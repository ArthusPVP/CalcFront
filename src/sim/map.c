/* map storage, ownership changes, shoreline queries, water pathfinding, lines */
#include "sim.h"

/* Hot paths in hand-written eZ80 (src/asm/map*.s): conquer(), scan_player(),
 * the route search loop and the coarse guide's breadth-first search. Their C
 * twins (suffix _c) serve the PC harness (and builds with -DCF_NOASM); the
 * self-test build (-DCF_SELFTEST) has both and compares them, map_selftest(). */
#if defined(HOST) || defined(CF_NOASM)
#define MAP_C_ONLY
#define conquer_c conquer
#define scan_player_c scan_player
#define search_loop_c search_loop
#define bfs_loop_c bfs_loop
#endif
#if defined(MAP_C_ONLY) || defined(CF_SELFTEST)
#define MAP_TWINS
#endif
#ifdef CF_SELFTEST
void conquer_c(tile_t t, uint8_t p);
void scan_player_c(uint8_t p);
#endif

uint16_t g_W, g_H;
uint16_t g_rowStart[MAP_MAX_H + 1];
uint16_t g_rowOfHi[256];
int16_t g_dirofs[4];
map_info_t g_mi;

uint16_t ty(tile_t t) { return ty_in(t); }
uint16_t tx(tile_t t) { return (uint16_t)(t - g_rowStart[ty_in(t)]); }
uint16_t g_bitHi[256];
uint8_t g_bitLo[256], g_bitMask[256];

void sim_tables(void)
{
    unsigned i;
    for (i = 0; i < 256; i++) {
        g_bitHi[i] = (uint16_t)(i * 32);
        g_bitLo[i] = (uint8_t)(i >> 3);
        g_bitMask[i] = (uint8_t)(1 << (i & 7));
    }
}
map_nation_t g_nat[MAX_MAP_NATIONS];

const uint32_t g_bits[32] = {
    0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80,
    0x100, 0x200, 0x400, 0x800, 0x1000, 0x2000, 0x4000, 0x8000,
    0x10000, 0x20000, 0x40000, 0x80000, 0x100000, 0x200000, 0x400000, 0x800000,
    0x1000000, 0x2000000, 0x4000000, 0x8000000, 0x10000000, 0x20000000, 0x40000000,
    0x80000000};

/* file header, see tools/mapconv.py */
#define H_NAME 4
#define H_COMP 20
#define H_NVAR 21
#define H_VARS 22
#define VAR_LEN 10
#define NAT_LEN (4 + NATION_NAME_LEN)

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static const uint8_t *open_checked(uint8_t idx, uint16_t *size)
{
    const uint8_t *d = plat_map_open(idx, size);
    if (!d || *size < H_VARS + VAR_LEN + 1 || memcmp(d, "CFM2", 4) != 0) return 0;
    return d;
}

uint8_t map_variants(uint8_t idx)
{
    uint16_t size;
    const uint8_t *d = open_checked(idx, &size);
    return d ? d[H_NVAR] : 0;
}

static void set_geometry(uint16_t w, uint16_t h)
{
    uint16_t y, b;
    g_W = w;
    g_H = h;
    for (y = 0; y <= h; y++) g_rowStart[y] = (uint16_t)(y * w);
    for (b = 0; b < 256; b++) {
        uint32_t row = ((uint32_t)b << 8) / w;
        g_rowOfHi[b] = (uint16_t)(row >= h ? (uint32_t)h - 1 : row);
    }
    g_dirofs[0] = 1;
    g_dirofs[1] = -1;
    g_dirofs[2] = (int16_t)w;
    g_dirofs[3] = (int16_t)-w;
}

bool map_load_info(uint8_t idx, uint8_t var)
{
    uint16_t size, i;
    const uint8_t *d = open_checked(idx, &size), *v, *n;
    if (!d) return false;
    memcpy(g_mi.name, d + H_NAME, MAP_NAME_LEN);
    g_mi.name[MAP_NAME_LEN - 1] = 0;
    g_mi.comp = d[H_COMP];
    g_mi.nvar = d[H_NVAR];
    if (var >= g_mi.nvar) var = (uint8_t)(g_mi.nvar - 1);
    g_mi.var = var;
    v = d + H_VARS + var * VAR_LEN;
    g_mi.w = rd16(v);
    g_mi.h = rd16(v + 2);
    g_mi.land = rd16(v + 4);
    g_mi.scale16 = rd16(v + 6);
    g_mi.off = rd16(v + 8);
    n = d + H_VARS + g_mi.nvar * VAR_LEN;
    g_mi.nnations = n[0] > MAX_MAP_NATIONS ? MAX_MAP_NATIONS : n[0];
    n++;
    for (i = 0; i < g_mi.nnations; i++, n += NAT_LEN) {
        g_nat[i].x = (uint16_t)(((uint32_t)rd16(n) * g_mi.w) >> 16);
        g_nat[i].y = (uint16_t)(((uint32_t)rd16(n + 2) * g_mi.h) >> 16);
        memcpy(g_nat[i].name, n + 4, NATION_NAME_LEN);
        g_nat[i].name[NATION_NAME_LEN - 1] = 0;
    }
    set_geometry(g_mi.w, g_mi.h);
    return true;
}

/* the tiles in g_map are exactly map file idx, variant var, as loaded: no
 * owner, fallout or other change yet (cleared by anything that writes tiles) */
bool g_mapClean;
static uint8_t cleanIdx = 0xFF, cleanVar;
static uint16_t cleanLand;
static uint8_t mapIdx; /* map file of the tiles in g_map */

bool map_load_tiles(uint8_t idx, uint8_t var)
{
    uint16_t size;
    const uint8_t *d;
    if (!map_load_info(idx, var)) return false;
    memset(g_stbits, 0, 8192);
    memset(g_railbits, 0, sizeof g_mem->railbits);
    mapIdx = idx;
    if (g_mapClean && cleanIdx == idx && cleanVar == g_mi.var) {
        g_mi.land = cleanLand; /* already there: no need to unpack it again */
        return true;
    }
    d = plat_map_open(idx, &size);
    plat_map_tiles(d + g_mi.off, (uint16_t)(g_mi.w * g_mi.h), g_mi.comp);
    /* count the land from the tiles themselves: the share of the map shown to
     * the player and the win check must never include water */
    {
        const uint8_t *p = g_map, *end = g_map + (uint16_t)(g_mi.w * g_mi.h);
        uint16_t n = 0;
        for (; p < end; p++)
            if (IS_LAND(*p)) n++;
        g_mi.land = cleanLand = n;
    }
    g_mapClean = true;
    cleanIdx = idx;
    cleanVar = g_mi.var;
    return true;
}

bool has_struct(tile_t t) { return bitget(g_stbits, t) != 0; }

void st_set(tile_t t, bool on)
{
    if (on)
        bitset(g_stbits, t);
    else
        bitclr(g_stbits, t);
}

/* coast[p][c]: pairs (land tile of p, water tile of body c next to it), kept
 * up to date on every change of owner so coast_bodies() needs no scan */
__attribute__((used)) uint16_t map_coast[MAXP][8]; /* (src/asm/mapconq.s) */
#define coast map_coast

static void coast_move(tile_t t, uint8_t from, uint8_t to)
{
    uint8_t i;
    for (i = 0; i < 4; i++) {
        uint8_t nb = g_map[(tile_t)(t + g_dirofs[i])];
        if (IS_LAND(nb) || !(nb & T_COMP)) continue;
        if (from) coast[from][nb & T_COMP]--;
        if (to) coast[to][nb & T_COMP]++;
    }
}

/* recount every coast (loaded game; a new game owns nothing: just clear) */
void coast_rebuild(bool scan)
{
    uint16_t t, n = (uint16_t)(g_W * g_H);
    memset(coast, 0, sizeof coast);
    if (!scan) return;
    for (t = 0; t < n; t++) {
        uint8_t b = g_map[t];
        if (IS_LAND(b) && (b & T_OWNER)) coast_move(t, 0, b & T_OWNER);
    }
}

#ifdef MAP_TWINS
void conquer_c(tile_t t, uint8_t p)
{
    uint8_t b = g_map[t], old = b & T_OWNER, i;
    uint16_t x, y;
    player_t *pl = &g_pl[p];
    if (old == p || !IS_LAND(b)) return;
    g_mapClean = false;
    if (old) g_pl[old].tiles--;
    if (b & T_FALL) g_landLeft++;
    coast_move(t, old, p);
    g_map[t] = (uint8_t)((b & T_TERR) | p);
    pl->tiles++;
    y = ty_in(t);
    x = (uint16_t)(t - g_rowStart[y]);
    if (x < pl->minx) pl->minx = x;
    if (x > pl->maxx) pl->maxx = x;
    if (y < pl->miny) pl->miny = y;
    if (y > pl->maxy) pl->maxy = y;
    for (i = 0; i < 4; i++) {
        uint8_t nb = g_map[(tile_t)(t + g_dirofs[i])];
        if (IS_LAND(nb)) {
            uint8_t o = nb & T_OWNER;
            if (o == p) continue;
            if (o) {
                MASKSET(pl->nbrs, o);
                MASKSET(g_pl[o].nbrs, p);
            } else
                pl->nbrTN = 1;
        }
    }
    ui_dirty(t);
    if (has_struct(t)) unit_on_conquer(t, p);
    if (old && g_pl[old].tiles == 0) kill_player(old, p);
}
#endif
#ifndef MAP_C_ONLY
/* player_t fields read by the assembly (src/asm/mapconq.s) */
_Static_assert(offsetof(player_t, tiles) == 20 && offsetof(player_t, minx) == 22, "player_t");
_Static_assert(offsetof(player_t, maxx) == 24 && offsetof(player_t, miny) == 26, "player_t");
_Static_assert(offsetof(player_t, maxy) == 28 && offsetof(player_t, nbrs) == 38, "player_t");
_Static_assert(offsetof(player_t, nbrTN) == 46 && sizeof(player_t) == 256, "player_t");
_Static_assert(sizeof map_coast == MAXP * 16 && T_TERR == 0x60 && T_OWNER == 31, "map_coast");
#endif

/* tile destroyed by a nuke: nobody owns it, fallout */
void relinquish(tile_t t)
{
    uint8_t b = g_map[t], old = b & T_OWNER;
    if (!IS_LAND(b)) return;
    g_mapClean = false;
    if (!(b & T_FALL)) g_landLeft--;
    if (old) coast_move(t, old, 0);
    g_map[t] = (uint8_t)((b & T_TERR) | T_FALL);
    ui_dirty(t);
    if (old) {
        g_pl[old].tiles--;
        if (g_pl[old].tiles == 0) kill_player(old, 0);
    }
}

uint8_t shore_comp(tile_t t)
{
    uint8_t i;
    for (i = 0; i < 4; i++) {
        uint8_t nb = g_map[(tile_t)(t + g_dirofs[i])];
        if (!IS_LAND(nb) && (nb & T_COMP)) return nb & T_COMP;
    }
    return 0;
}

bool is_shore(tile_t t) { return IS_LAND(g_map[t]) && shore_comp(t) != 0; }

tile_t water_next_to(tile_t t, uint8_t comp)
{
    uint8_t i;
    if (!comp) return NO_TILE;
    for (i = 0; i < 4; i++) {
        tile_t n = (tile_t)(t + g_dirofs[i]);
        uint8_t nb = g_map[n];
        if (!IS_LAND(nb) && (nb & T_COMP) == comp) return n;
    }
    return NO_TILE;
}

#ifdef MAP_TWINS
void scan_player_c(uint8_t p)
{
    player_t *pl = &g_pl[p];
    uint16_t x, y, x0 = pl->minx, x1 = pl->maxx, y0 = pl->miny, y1 = pl->maxy;
    uint16_t nx0 = 0xFFFF, nx1 = 0, ny0 = 0xFFFF, ny1 = 0;
    uint8_t i, tn = 0;
    uint32_t mask = 0;
    if (!pl->tiles) return;
    for (y = y0;; y++) {
        const uint8_t *row = g_map + g_rowStart[y];
        for (x = x0;; x++) {
            uint8_t b = row[x];
            if ((b & T_OWNER) == p && IS_LAND(b)) {
                tile_t t = TILE(x, y);
                if (x < nx0) nx0 = x;
                if (x > nx1) nx1 = x;
                if (y < ny0) ny0 = y;
                ny1 = y;
                for (i = 0; i < 4; i++) {
                    uint8_t nb = g_map[(tile_t)(t + g_dirofs[i])];
                    if (IS_LAND(nb)) {
                        uint8_t o = nb & T_OWNER;
                        if (o == p) continue;
                        if (o)
                            MASKSET(mask, o);
                        else
                            tn = 1;
                    }
                }
            }
            if (x == x1) break;
        }
        if (y == y1) break;
    }
    pl->nbrs = mask;
    pl->nbrTN = tn;
    if (nx0 <= nx1) {
        pl->minx = nx0;
        pl->maxx = nx1;
        pl->miny = ny0;
        pl->maxy = ny1;
    }
}
#endif

tile_t random_land(void)
{
    uint8_t tries = 200;
    while (tries--) {
        tile_t t = rnd_tile(0, g_W, 0, g_H);
        if (IS_LAND(g_map[t])) return t;
    }
    return NO_TILE;
}

tile_t random_water_near(tile_t c, uint8_t r, uint8_t comp)
{
    uint8_t tries = 24;
    int16_t cx = (int16_t)tx(c), cy = (int16_t)ty(c);
    while (tries--) {
        int16_t x = (int16_t)(cx + rnd_n(2 * r + 1) - r), y = (int16_t)(cy + rnd_n(2 * r + 1) - r);
        tile_t t;
        if (x < 1 || y < 1 || x >= (int16_t)g_W - 1 || y >= (int16_t)g_H - 1) continue;
        t = TILE(x, y);
        if (!IS_LAND(g_map[t]) && (g_map[t] & T_COMP) == comp) return t;
    }
    return NO_TILE;
}

/* own land tile next to water body `comp`, closest to `to` (Manhattan) */
tile_t nearest_own_shore(uint8_t p, tile_t to, uint8_t comp)
{
    player_t *pl = &g_pl[p];
    uint16_t x, y, tox = tx(to), toy = ty(to);
    int16_t W = (int16_t)g_W;
    tile_t best = NO_TILE;
    uint16_t bd = 0xFFFF;
    if (!pl->tiles) return NO_TILE;
    for (y = pl->miny;; y++) {
        uint16_t dy = y > toy ? y - toy : toy - y;
        if (dy < bd) { /* rows farther than the best so far cannot win */
            const uint8_t *row = g_map + g_rowStart[y];
            for (x = pl->minx;; x++) {
                const uint8_t *q = row + x;
                uint8_t b = *q;
                if (IS_LAND(b) && (b & T_OWNER) == p &&
                    ((q[1] & (T_TERR | T_COMP)) == comp || (q[-1] & (T_TERR | T_COMP)) == comp ||
                     (q[W] & (T_TERR | T_COMP)) == comp || (q[-W] & (T_TERR | T_COMP)) == comp)) {
                    uint16_t d = (uint16_t)((x > tox ? x - tox : tox - x) + dy);
                    if (d < bd) {
                        bd = d;
                        best = (tile_t)(q - g_map);
                    }
                }
                if (x == pl->maxx) break;
            }
        }
        if (y == pl->maxy) break;
    }
    return best;
}

/* navigable water bodies touching the land of p (bit c = water body c) */
uint8_t coast_bodies(uint8_t p)
{
    uint8_t c, mask = 0;
    for (c = 1; c < 8; c++)
        if (coast[p][c]) mask |= (uint8_t)(1 << c);
    return mask;
}

/* -------------------------------------------------------------- lines --- */
uint16_t line_len(tile_t a, tile_t b)
{
    int ya = ty(a), yb = ty(b);
    int dx = (int)(b - g_rowStart[yb]) - (int)(a - g_rowStart[ya]), dy = yb - ya;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return (uint16_t)(dx > dy ? dx : dy);
}

/* k-th of n steps from a to b, rounded. |dx| * k * 2 stays far below 2^23
 * (map sides <= 512, n <= a few hundred), so 24-bit ints suffice. */
tile_t line_point(tile_t a, tile_t b, uint16_t k, uint16_t n)
{
    int ay = ty(a), by = ty(b), ax = (int)(a - g_rowStart[ay]);
    int dx = (int)(b - g_rowStart[by]) - ax, dy = by - ay, x, y, nn = (int)n;
    if (!n) return a;
    x = ax + (dx * (int)k * 2 + (dx >= 0 ? nn : -nn)) / (2 * nn);
    y = ay + (dy * (int)k * 2 + (dy >= 0 ? nn : -nn)) / (2 * nn);
    return TILE((uint16_t)x, (uint16_t)y);
}

/* ------------------------------------------------------------ pathing --- */
/* Paths are shared: ships sailing between the same two tiles reuse one
 * route, and finished routes stay cached until their slot is needed. */
/* the tile search's heap spans hq and hk */
typedef char hq_hk_follow[offsetof(sim_mem_t, hk) == offsetof(sim_mem_t, hq) + 2 * HEAPN ? 1 : -1];
#define hq (g_mem->hq)
#define hk (g_mem->hk)
#define vlist (g_mem->vlist)
#ifdef PATH_STATS
unsigned long g_pathPops, g_pathCalls, g_pathFails, g_pathMax, g_pathBfs, g_pathStates, g_pathOverflow;
#endif
/* the tile search's state, shared with its loop (src/asm/mapsrch.s) */
typedef struct {
    tile_t to;         /* goal */
    uint16_t gx, gy;   /* its column and row */
    uint8_t comp;      /* water body */
    uint8_t guided;    /* keys from the coarse guide */
    uint16_t budget;   /* tiles to expand at most */
    uint16_t pops;     /* out: tiles expanded */
    uint16_t hn, vn;   /* heap entries, visited tiles listed in vlist */
} srch_t;
__attribute__((used)) srch_t map_srch;
#define hn (map_srch.hn)
#define vn (map_srch.vn)
static uint8_t pathbuf[MAX_PATHS][PATH_STEPS / 4];
static uint16_t pathlen[MAX_PATHS];
static tile_t pathFrom[MAX_PATHS], pathTo[MAX_PATHS];
static uint8_t pathRefs[MAX_PATHS];
static uint8_t pathOk[MAX_PATHS]; /* 0 empty, 1 complete, 2 truncated */
static uint8_t pathAge[MAX_PATHS];
static uint8_t pathClock;

void path_reset(void)
{
    memset(pathRefs, 0, sizeof pathRefs);
    memset(pathOk, 0, sizeof pathOk);
}

void path_release(uint8_t slot)
{
    if (slot < MAX_PATHS && pathRefs[slot]) pathRefs[slot]--;
}

int8_t path_step(uint8_t slot, uint16_t idx)
{
    if (slot >= MAX_PATHS || idx >= pathlen[slot]) return -1;
    return (int8_t)((pathbuf[slot][idx >> 2] >> ((idx & 3) * 2)) & 3);
}

bool path_complete(uint8_t slot) { return slot < MAX_PATHS && pathOk[slot] == 1; }

#define W_VIS 0x80
#define W_DIRSH 3

/* ------------------------------------------------------- coarse guide --- */
/* The map is cut into 8x8 tile cells. Inside a cell, the water forms one or
 * a few separate pools (a headland or an isthmus splits them). Before a long
 * search, a breadth-first search over (cell, pool) from the goal counts the
 * cells left to sail, crossing a cell border only where water of both pools
 * really touches. The tile search then always expands the tile with the
 * fewest cells left, so it follows the sea around a continent instead of
 * filling every bay that points at the goal.
 * Sides: 0 right, 1 left, 2 down, 3 up (same order as g_dirofs). */
#define CS 8
#define CMAX 1100
#define NSPLIT 128
/* (global: the assembly, src/asm/mapsrch.s, reads them) */
#define cCross map_cCross
#define cSplit map_cSplit
#define cDist map_cDist
#define cPar map_cPar
#define splitLab map_splitLab
#define splitSides map_splitSides
#define splitDist map_splitDist
#define splitPar map_splitPar
#define cRow map_cRow
#define cW map_cW
#define gPool map_gPool
#define cGoal map_cGoal
__attribute__((used)) uint8_t cCross[CMAX]; /* bits 0-2 water body across the right border, 4-6 bottom */
__attribute__((used)) uint8_t cSplit[CMAX]; /* 0 (one pool at most), or 1 + index into split* */
__attribute__((used)) uint8_t cDist[CMAX];  /* cells left from the cell's pool, 255 = unknown */
__attribute__((used)) uint8_t cPar[CMAX];   /* side toward the goal */
__attribute__((used)) uint8_t splitLab[NSPLIT][16];  /* pool (0-3) of each tile, 2 bits per tile */
__attribute__((used)) uint8_t splitSides[NSPLIT][4]; /* sides each pool crosses */
__attribute__((used)) uint8_t splitDist[NSPLIT][4], splitPar[NSPLIT];
static uint8_t nSplit;
__attribute__((used)) uint16_t cRow[MAP_MAX_H / CS + 1];
__attribute__((used)) uint8_t cW, gPool;
static uint8_t cH;
static uint16_t cN;
__attribute__((used)) uint16_t cGoal;
static bool cOn;  /* the grid fits this map */
static bool cUse; /* the running search is guided */
#ifdef CF_SELFTEST
static bool stUseC;       /* map_selftest(): the C twins in place of the assembly */
static uint16_t stStates; /* states of the last coarse_bfs() */
#endif

static const uint8_t sideBit[4] = {1, 2, 4, 8};

static uint8_t cell_size(uint16_t org, uint16_t lim) { return lim - org < CS ? (uint8_t)(lim - org) : CS; }

static int cellRow[CS]; /* ly * W: offsets of a cell's rows from its top-left tile */

/* Flood the water of a cell (top-left tile at base, size w x h) from local
 * tile s (ly * 8 + lx), setting lab[] to id. Returns the sides where it
 * crosses into the next cell. The map frame is never navigable, so the
 * neighbours of a navigable tile always exist. */
__attribute__((noinline)) static uint8_t cell_fill(const uint8_t *base, uint8_t w, uint8_t h, uint8_t s,
                                                   uint8_t *lab, uint8_t id)
{
    static const int8_t sideDx[4] = {1, -1, 0, 0}, sideDy[4] = {0, 0, 1, -1};
    uint8_t stk[64], n = 0, sides = 0;
    int W = (int)g_W;
    lab[s] = id;
    stk[n++] = s;
    while (n) {
        uint8_t i = stk[--n], lx = i & 7, ly = i >> 3, k;
        const uint8_t *t = base + cellRow[ly] + lx;
        uint8_t comp = *t & T_COMP;
        for (k = 0; k < 4; k++) {
            int8_t nx = (int8_t)(lx + sideDx[k]), ny = (int8_t)(ly + sideDy[k]);
            uint8_t nb = t[k == 0 ? 1 : k == 1 ? -1 : k == 2 ? W : -W];
            if ((nb & (T_TERR | T_COMP)) != comp) continue; /* land or another sea */
            if (nx < 0 || ny < 0 || nx >= (int8_t)w || ny >= (int8_t)h) {
                sides |= sideBit[k];
                continue;
            }
            {
                uint8_t j = (uint8_t)((ny << 3) | nx);
                if (lab[j]) continue;
                lab[j] = id;
                stk[n++] = j;
            }
        }
    }
    return sides;
}

/* water body crossing the right (right) or bottom border of a cell made of
 * one sea c0 only, or 0 */
static uint8_t open_cross(const uint8_t *base, uint8_t w, uint8_t h, uint8_t c0, bool right)
{
    uint8_t k;
    for (k = 0; k < (right ? h : w); k++)
        if ((right ? base[cellRow[k] + w] : base[(int)g_W * h + k]) == c0) return c0;
    return 0;
}

/* The guide only depends on the map's water, so it is kept for as long as
 * the same map is played. It is built one row of cells per call to
 * path_prepare_step(): the calculator does it while the player picks a
 * start, and any route search finishes it first if needed. */
static uint8_t prepRow, guideIdx = 0xFF, guideVar;
static bool prepDone;

void path_prepare_begin(void)
{
    prepDone = guideIdx == mapIdx && guideVar == g_mi.var;
    if (prepDone) return;
    guideIdx = 0xFF;
    cW = (uint8_t)((g_W + CS - 1) / CS);
    cH = (uint8_t)((g_H + CS - 1) / CS);
    cN = (uint16_t)cW * cH;
    cOn = cN <= CMAX;
    nSplit = 0;
    prepRow = 0;
    prepDone = !cOn;
    {
        uint8_t k;
        for (k = 0; k < CS; k++) cellRow[k] = (int)g_W * k;
    }
}

void path_ready(void)
{
    while (!path_prepare_step())
        ;
}

void path_prepare(void)
{
    path_prepare_begin();
    path_ready();
}

bool path_prepare_step(void)
{
    uint8_t cx, cy = prepRow;
    uint16_t c;
    if (prepDone) return true;
    {
        uint16_t y0 = (uint16_t)cy * CS;
        uint8_t h = cell_size(y0, g_H);
        c = (uint16_t)cy * cW;
        cRow[cy] = c;
        for (cx = 0; cx < cW; cx++, c++) {
            uint16_t x0 = (uint16_t)cx * CS;
            const uint8_t *base = g_map + g_rowStart[y0] + x0;
            uint8_t w = cell_size(x0, g_W), lab[64], i, cross = 0, pools = 0, c0 = *base, lx, ly;
            uint8_t sides[4] = {0, 0, 0, 0};
            cSplit[c] = 0;
            /* open sea: the whole cell is one water body (most cells) */
            if ((c0 & T_COMP) && !(c0 & T_TERR)) {
                bool same = true;
                for (ly = 0; ly < h && same; ly++) {
                    const uint8_t *r = base + cellRow[ly];
                    for (lx = 0; lx < w; lx++)
                        if (r[lx] != c0) {
                            same = false;
                            break;
                        }
                }
                if (same) {
                    cCross[c] = (uint8_t)(open_cross(base, w, h, c0, true) |
                                          (open_cross(base, w, h, c0, false) << 4));
                    continue;
                }
            }
            memset(lab, 0, sizeof lab);
            for (ly = 0; ly < h; ly++) {
                const uint8_t *r = base + cellRow[ly];
                for (lx = 0; lx < w; lx++) {
                    uint8_t b = r[lx], sd;
                    i = (uint8_t)((ly << 3) | lx);
                    if (lab[i] || (b & T_TERR) || !(b & T_COMP)) continue;
                    if (pools < 4) pools++; /* a 5th pool shares the 4th one's slot */
                    sd = cell_fill(base, w, h, i, lab, pools);
                    sides[pools - 1] |= sd;
                    if ((sd & 1) && !(cross & 0x07)) cross |= b & T_COMP;
                    if ((sd & 4) && !(cross & 0x70)) cross |= (uint8_t)((b & T_COMP) << 4);
                }
            }
            cCross[c] = cross;
            if (pools > 1 && nSplit < NSPLIT) {
                uint8_t *m = splitLab[nSplit];
                memset(m, 0, 16);
                for (i = 0; i < 64; i++)
                    if (lab[i]) m[i >> 2] |= (uint8_t)(((lab[i] - 1) & 3) << ((i & 3) * 2));
                memcpy(splitSides[nSplit], sides, 4);
                cSplit[c] = ++nSplit;
            }
        }
    }
    if (++prepRow == cH) {
        prepDone = true;
        guideIdx = mapIdx;
        guideVar = g_mi.var;
    }
    return prepDone;
}

#ifdef HOST
uint16_t path_guide_hash(void)
{
    uint16_t c, h = (uint16_t)(cN + nSplit * 7);
    uint8_t k;
    for (c = 0; c < cN; c++) h = (uint16_t)(h * 31 + cCross[c] * 7 + cSplit[c]);
    for (k = 0; k < nSplit; k++) {
        uint8_t i;
        for (i = 0; i < 16; i++) h = (uint16_t)(h * 31 + splitLab[k][i]);
        for (i = 0; i < 4; i++) h = (uint16_t)(h * 31 + splitSides[k][i]);
    }
    return h;
}
#endif

/* bits 2k and 2k+1 of b, by masks and compares: a shift by more than one bit
 * is a library loop on the eZ80 */
FORCE_INLINE uint8_t pair_at(uint8_t b, uint8_t k)
{
    uint8_t m;
    switch (k) {
    case 0: return b & 3;
    case 1: m = b & 0x0C; return m == 0 ? 0 : m == 0x04 ? 1 : m == 0x08 ? 2 : 3;
    case 2: m = b & 0x30; return m == 0 ? 0 : m == 0x10 ? 1 : m == 0x20 ? 2 : 3;
    default: m = b & 0xC0; return m == 0 ? 0 : m == 0x40 ? 1 : m == 0x80 ? 2 : 3;
    }
}

/* pool of local tile (lx, ly) of cell c: 2 bits of byte ly * 2 + lx / 4
 * (a call: split cells are the rare case) */
static uint8_t pool_at(uint16_t c, uint8_t lx, uint8_t ly)
{
    uint8_t k = cSplit[c];
    const uint8_t *m;
    if (!k) return 0;
    m = splitLab[k - 1] + ly + ly;
    if (lx > 3) m++;
    return pair_at(*m, lx & 3);
}

static const uint8_t parMul[4] = {1, 4, 16, 64};          /* 1 << (pool * 2) */
static const uint8_t parClr[4] = {0xFC, 0xF3, 0xCF, 0x3F}; /* ~(3 << (pool * 2)) */

static void node_set(uint16_t c, uint8_t pool, uint8_t d, uint8_t side)
{
    uint8_t k = cSplit[c];
    if (k) {
        splitDist[k - 1][pool] = d;
        splitPar[k - 1] = (uint8_t)((splitPar[k - 1] & parClr[pool]) | side * parMul[pool]);
    } else {
        cDist[c] = d;
        cPar[c] = side;
    }
}

/* v >> 3 for v < 2048, from the bitmap tables (no shift loops) */
FORCE_INLINE uint8_t div8(uint16_t v)
{
    u16b_t u;
    u.v = v;
    return (uint8_t)(g_bitHi[u.b[1]] + g_bitLo[u.b[0]]);
}

FORCE_INLINE uint16_t cell_of(uint16_t x, uint16_t y) { return cRow[div8(y)] + div8(x); }

/* pools of cell nc (bit per pool) touching pool p of cell c across side a */
#define border_pools map_border_pools
__attribute__((used)) uint8_t border_pools(uint16_t c, uint8_t p, uint8_t a, uint16_t nc)
{
    uint8_t cy = (uint8_t)(c / cW), w, h, n, lx = 0, ly = 0, nlx, nly, pools = 0;
    uint16_t x0 = (uint16_t)(c - cRow[cy]) * CS, y0 = (uint16_t)cy * CS;
    const uint8_t *m;
    int off = g_dirofs[a], step = a < 2 ? (int)g_W : 1;
    w = cell_size(x0, g_W);
    h = cell_size(y0, g_H);
    n = a < 2 ? h : w;
    switch (a) {
    case 0: lx = (uint8_t)(w - 1); break;
    case 2: ly = (uint8_t)(h - 1); break;
    }
    /* the matching tile across the border: same row or column, other edge */
    nlx = a == 0 ? 0 : a == 1 ? 7 : lx;
    nly = a == 2 ? 0 : a == 3 ? 7 : ly;
    for (m = g_map + TILE(x0 + lx, y0 + ly); n; n--, m += step) {
        uint8_t bu = *m & (T_TERR | T_COMP);
        if (bu && !(bu & T_TERR) && bu == (m[off] & (T_TERR | T_COMP)) && pool_at(c, lx, ly) == p)
            pools |= sideBit[pool_at(nc, nlx, nly)];
        if (a < 2) {
            ly++;
            nly++;
        } else {
            lx++;
            nlx++;
        }
    }
    return pools;
}

/* The breadth-first search over (cell, pool), the goal's already queued:
 * cells in hq, their pools in hk (hq, hk and vlist follow each other); each
 * (cell, pool) is queued once, at most CMAX + 3 * NSPLIT. It stops a few
 * layers past the start's pool (sc, sPool). Returns the states dequeued.
 * Assembly on the calculator (src/asm/mapbfs.s). */
#ifdef MAP_TWINS
uint16_t bfs_loop_c(uint16_t sc, uint8_t sPool, uint8_t comp)
{
    uint16_t *q = hq;
    uint8_t *qp = (uint8_t *)hk;
    uint16_t qh = 0, qt = 1, step[4], states = 0;
    uint8_t stop = 255, comp4 = (uint8_t)(comp * 16);
    step[0] = 1;
    step[1] = 0xFFFF;
    step[2] = cW;
    step[3] = (uint16_t)-cW;
    while (qh < qt) {
        uint16_t c = q[qh];
        uint8_t p = qp[qh], k = cSplit[c], d, nd, a, sides;
        qh++;
        states++;
        d = k ? splitDist[k - 1][p] : cDist[c];
        if (c == sc && p == sPool && stop == 255) stop = (uint8_t)(d > 250 ? 253 : d + 3);
        if (d >= stop) break;
        nd = d < 254 ? (uint8_t)(d + 1) : 254;
        if (k)
            sides = splitSides[k - 1][p];
        else {
            /* one pool: the sides where our sea crosses (bits 0-2 right,
             * 4-6 bottom; the cell before a row start is at the map edge) */
            uint8_t r = cCross[c];
            sides = 0;
            if ((r & 7) == comp) sides = 1;
            if (c && (cCross[c - 1] & 7) == comp) sides |= 2;
            if ((r & 0xF0) == comp4) sides |= 4;
            if (c >= cW && (cCross[c - cW] & 0xF0) == comp4) sides |= 8;
        }
        for (a = 0; sides; a++, sides >>= 1) {
            uint16_t nc;
            uint8_t pools, np, kn, par, *sd;
            if (!(sides & 1)) continue;
            nc = (uint16_t)(c + step[a]);
            kn = cSplit[nc];
            if (!kn) {
                /* one pool there, and pool p crosses side a: they touch */
                if (cDist[nc] == 255) {
                    cDist[nc] = nd;
                    cPar[nc] = a ^ 1;
                    q[qt] = nc;
                    qp[qt++] = 0;
                }
                continue;
            }
            sd = splitDist[kn - 1];
            if (!k) {
                /* we are one pool: the neighbour's pools that cross back to us */
                static const uint8_t backBit[4] = {2, 1, 8, 4}; /* sideBit[a ^ 1] */
                uint8_t back = backBit[a];
                pools = 0;
                if (splitSides[kn - 1][0] & back) pools = 1;
                if (splitSides[kn - 1][1] & back) pools |= 2;
                if (splitSides[kn - 1][2] & back) pools |= 4;
                if (splitSides[kn - 1][3] & back) pools |= 8;
            } else
                pools = border_pools(c, p, a, nc); /* split on both sides: look */
            par = splitPar[kn - 1];
            for (np = 0; pools; np++, pools >>= 1) {
                if (!(pools & 1) || sd[np] != 255) continue;
                sd[np] = nd;
                par = (uint8_t)((par & parClr[np]) | (a ^ 1) * parMul[np]);
                q[qt] = nc;
                qp[qt++] = np;
            }
            splitPar[kn - 1] = par;
        }
    }
    return states;
}
#endif
#ifndef MAP_C_ONLY
uint16_t bfs_loop(uint16_t sc, uint8_t sPool, uint8_t comp);
_Static_assert(sizeof(sim_mem_t) == 8192 + 3 * 4096 && sizeof splitSides[0] == 4, "bfs"); /* hk at 12288 */
#ifdef CF_SELFTEST
uint16_t bfs_loop_c(uint16_t sc, uint8_t sPool, uint8_t comp);
#endif
#endif

/* number the cells from the goal outward, until a few layers past the start */
static void coarse_bfs(tile_t from, tile_t to, uint8_t comp)
{
    uint16_t gy = ty(to), gx = (uint16_t)(to - g_rowStart[gy]), states;
    uint16_t sy = ty(from), sx = (uint16_t)(from - g_rowStart[sy]), sc = cell_of(sx, sy);
    uint8_t sPool = pool_at(sc, (uint8_t)sx & 7, (uint8_t)sy & 7);
    cGoal = cell_of(gx, gy);
    gPool = pool_at(cGoal, (uint8_t)gx & 7, (uint8_t)gy & 7);
    memset(cDist, 255, cN);
    memset(splitDist, 255, (size_t)nSplit * 4);
    node_set(cGoal, gPool, 0, 0);
    hq[0] = cGoal;
    ((uint8_t *)hk)[0] = gPool;
#ifdef CF_SELFTEST
    states = stUseC ? bfs_loop_c(sc, sPool, comp) : bfs_loop(sc, sPool, comp);
    stStates = states;
#else
    states = bfs_loop(sc, sPool, comp);
#endif
#ifdef PATH_STATS
    g_pathBfs++;
    g_pathStates += states;
#else
    (void)states;
#endif
}

#ifdef MAP_TWINS /* the route search loop's helpers (the C twin only) */
/* guided search key of the tile at (lx, ly) of a pool d cells from the goal's
 * (side par toward it), m tiles from the goal: 16 per cell left plus the
 * distance to the cell side toward the goal */
FORCE_INLINE uint16_t cell_key(uint8_t d, uint8_t par, uint8_t lx, uint8_t ly, uint16_t m)
{
    uint8_t b;
    if (d == 255) return (uint16_t)(0xF000 | (m > 0xFFF ? 0xFFF : m));
    switch (par) {
    case 0: b = (uint8_t)(7 - lx); break;
    case 1: b = lx; break;
    case 2: b = (uint8_t)(7 - ly); break;
    default: b = ly;
    }
    return (uint16_t)(d * 16 + b);
}

/* the same in cell c, split into pools (k = cSplit[c]) */
static uint16_t split_key(uint16_t c, uint8_t k, uint8_t lx, uint8_t ly, uint16_t m)
{
    uint8_t pool = pool_at(c, lx, ly);
    if (c == cGoal && pool == gPool) return m;
    return cell_key(splitDist[k - 1][pool], pair_at(splitPar[k - 1], pool), lx, ly, m);
}

FORCE_INLINE uint16_t guide_key(uint16_t c, uint8_t lx, uint8_t ly, uint16_t m)
{
    uint8_t k = cSplit[c];
    if (k) return split_key(c, k, lx, ly, m);
    if (c == cGoal) return m; /* gPool is 0 there */
    return cell_key(cDist[c], cPar[c], lx, ly, m);
}
#endif

/* The heap of the tile search: key and tile side by side, HEAPN entries over
 * hq and hk (they follow each other). */
typedef struct {
    uint16_t k;
    tile_t t;
} hent_t;
typedef char hent_size[sizeof(hent_t) == 4 ? 1 : -1];

#ifdef MAP_TWINS
/* (i - 1) / 2 for a heap index (a 16-bit shift by one is inline code) */
FORCE_INLINE uint16_t parent(uint16_t i) { return (uint16_t)(i - 1) >> 1; }

FORCE_INLINE void heap_push(hent_t *h, tile_t t, uint16_t k)
{
    uint16_t i = hn++;
    hent_t *e = h + i;
    while (i) {
        hent_t *pe;
        i = parent(i);
        pe = h + i;
        if (pe->k <= k) break;
        *e = *pe;
        e = pe;
    }
    e->k = k;
    e->t = t;
}

/* byte offsets (native 24-bit adds) in place of indexes: the children of
 * the entry at offset o are at 2 * o + 4 and 2 * o + 8 */
FORCE_INLINE tile_t heap_pop(hent_t *h)
{
    uint8_t *b = (uint8_t *)h;
    tile_t top = h[0].t;
    unsigned end = (unsigned)--hn * sizeof(hent_t), o = 0, c;
    hent_t last = *(hent_t *)(b + end);
    while ((c = o + o + sizeof(hent_t)) < end) {
        hent_t *ce = (hent_t *)(b + c);
        if (c + sizeof(hent_t) < end && ce[1].k < ce[0].k) {
            ce++;
            c += sizeof(hent_t);
        }
        if (ce->k >= last.k) break;
        *(hent_t *)(b + o) = *ce;
        o = c;
    }
    *(hent_t *)(b + o) = last;
    return top;
}
#endif

/* direction back to the parent, from a visited tile's scratch bits 3-4 */
FORCE_INLINE uint8_t back_of(uint8_t b)
{
    b &= 3 << W_DIRSH;
    return b == 0 ? 0 : b == 1 << W_DIRSH ? 1 : b == 2 << W_DIRSH ? 2 : 3;
}

/* box around the visited tiles vlist had no room for */
static uint16_t farX0, farX1, farY0, farY1;

__attribute__((used)) void map_note_far(tile_t t)
{
    uint16_t y = ty(t), x = (uint16_t)(t - g_rowStart[y]);
    if (x < farX0) farX0 = x;
    if (x > farX1) farX1 = x;
    if (y < farY0) farY0 = y;
    if (y > farY1) farY1 = y;
}

/* The search loop: pops the heap (map_srch.hn entries, the start tile
 * already in) until the goal, an empty heap or the end of the budget; 1 when
 * the goal is reached. Expanded tiles in map_srch.pops. Assembly on the
 * calculator (src/asm/mapsrch.s). */
#ifdef MAP_TWINS
uint8_t search_loop_c(void)
{
    /* one step in direction i (as g_dirofs): local x and y in the cell, and
     * the next cell when they leave it */
    static const uint8_t stepX[4] = {1, 0xFF, 0, 0}, stepY[4] = {0, 0, 1, 0xFF};
    static const uint8_t backBits[4] = {1 << W_DIRSH, 0, 3 << W_DIRSH, 2 << W_DIRSH};
    hent_t *h = (hent_t *)hq;
    uint16_t gx = map_srch.gx, gy = map_srch.gy, pc = 0, cStep[4], budget = map_srch.budget, pops = 0;
    uint8_t comp = map_srch.comp, i, plx = 0, ply = 0, ok = 0, guided = map_srch.guided;
    tile_t t, to = map_srch.to;

    cStep[0] = 1;
    cStep[1] = 0xFFFF;
    cStep[2] = cW;
    cStep[3] = (uint16_t)(0 - cW);
    while (hn) {
        uint16_t px, py, m;
        uint8_t tw = 0; /* directions toward the goal (bit per direction) */
        t = heap_pop(h);
        if (t == to) {
            ok = 1;
            break;
        }
        if (!budget--) break;
        pops++;
        py = ty_in(t);
        px = (uint16_t)(t - g_rowStart[py]);
        if (px < gx) tw = 1;
        if (px > gx) tw = 2;
        if (py < gy) tw |= 4;
        if (py > gy) tw |= 8;
        m = (uint16_t)((px > gx ? px - gx : gx - px) + (py > gy ? py - gy : gy - py));
        if (guided) {
            pc = cell_of(px, py);
            plx = (uint8_t)px & 7;
            ply = (uint8_t)py & 7;
        }
        for (i = 0; i < 4; i++) {
            tile_t nt = (tile_t)(t + g_dirofs[i]);
            uint8_t nb = g_map[nt], lx, ly;
            uint16_t c, nm;
            if ((nb & (T_TERR | W_VIS)) || (nb & T_COMP) != comp) continue;
            if (hn >= HEAPN) break; /* heap full: leave it to another neighbour */
            g_map[nt] = (uint8_t)(nb | W_VIS | backBits[i]); /* direction back: i ^ 1 */
            if (vn < VMAX)
                vlist[vn++] = nt;
            else
                map_note_far(nt);
            /* one step: one tile closer to or further from the goal */
            nm = (uint16_t)(m + 1 - ((tw & sideBit[i]) ? 2 : 0));
            if (guided) {
                lx = (uint8_t)(plx + stepX[i]);
                ly = (uint8_t)(ply + stepY[i]);
                c = pc;
                if ((uint8_t)(lx | ly) & 0xF8) { /* -1 or 8: into the next cell */
                    c += cStep[i];
                    lx &= 7;
                    ly &= 7;
                }
                nm = guide_key(c, lx, ly, nm);
            }
            heap_push(h, nt, nm);
        }
    }
    map_srch.pops = pops;
    return ok;
}
#endif
#ifndef MAP_C_ONLY
uint8_t search_loop(void);
/* layouts read by src/asm/mapsrch.s */
_Static_assert(offsetof(srch_t, to) == 0 && offsetof(srch_t, gx) == 2 && offsetof(srch_t, gy) == 4, "srch_t");
_Static_assert(offsetof(srch_t, comp) == 6 && offsetof(srch_t, guided) == 7, "srch_t");
_Static_assert(offsetof(srch_t, budget) == 8 && offsetof(srch_t, pops) == 10, "srch_t");
_Static_assert(sizeof(srch_t) == 16, "srch_t"); /* hn at 12, vn at 14 */
_Static_assert(offsetof(sim_mem_t, railbits) == 0 && sizeof(sim_mem_t) == 20480, "sim_mem_t"); /* hq at 8192, vlist at 16384 */
_Static_assert(HEAPN == 2048 && VMAX == 2048 && W_VIS == 0x80 && W_DIRSH == 3, "search");
_Static_assert(sizeof cRow[0] == 2 && sizeof splitLab[0] == 16 && sizeof splitDist[0] == 4, "guide");
#ifdef CF_SELFTEST
uint8_t search_loop_c(void);
#endif
#endif

/* Greedy best-first search over one water body, expanding at most `budget`
 * tiles. Each visited tile remembers the direction back to its parent. The
 * map frame is water body 0, so the search never leaves the map. Keys are
 * the Manhattan distance to the goal, or with the guide, guide_key(); the
 * cell and the distance of a new tile follow from the expanded tile's. */
static bool search(tile_t from, tile_t to, uint8_t slot, uint16_t budget)
{
    hent_t *h = (hent_t *)hq;
    uint16_t n = 0, x, y, k;
    bool ok, overflow;
    tile_t t;

    pathlen[slot] = 0;
    if (from == to) return true;

    map_srch.to = to;
    map_srch.gy = ty(to);
    map_srch.gx = (uint16_t)(to - g_rowStart[map_srch.gy]);
    map_srch.comp = g_map[from] & T_COMP;
    map_srch.guided = cUse;
    map_srch.budget = budget;
    vn = 0;
    farX0 = farY0 = 0xFFFF;
    farX1 = farY1 = 0;
    g_map[from] |= W_VIS;
    vlist[vn++] = from;
    h[0].k = 0;
    h[0].t = from;
    hn = 1;
#ifdef CF_SELFTEST
    ok = stUseC ? search_loop_c() : search_loop();
#else
    ok = search_loop();
#endif
#ifdef PATH_STATS
    g_pathPops += map_srch.pops;
#endif
    if (ok) {
        /* count the steps walking back from the goal, then store them forward
         * (step k in bits (k & 3) * 2 of byte k / 4) */
        uint8_t *pb = pathbuf[slot], sub;
        uint16_t bi;
        for (t = to; t != from; n++) t = (tile_t)(t + g_dirofs[back_of(g_map[t])]);
        memset(pb, 0, PATH_STEPS / 4);
        bi = n >> 2;
        sub = (uint8_t)n & 3;
        for (t = to, k = n; t != from;) {
            uint8_t back = back_of(g_map[t]);
            k--;
            if (!sub--) {
                sub = 3;
                bi--;
            }
            if (k < PATH_STEPS) pb[bi] |= (uint8_t)((back ^ 1) * parMul[sub]);
            t = (tile_t)(t + g_dirofs[back]);
        }
        pathlen[slot] = n > PATH_STEPS ? PATH_STEPS : n;
    }
    /* clear scratch bits */
    for (k = 0; k < vn; k++) g_map[vlist[k]] &= T_COMP;
    overflow = farX0 != 0xFFFF;
#ifdef PATH_STATS
    if (overflow) g_pathOverflow++;
#endif
    if (overflow)
        for (y = farY0;; y++) {
            uint8_t *row = g_map + g_rowStart[y];
            for (x = farX0;; x++) {
                if (!IS_LAND(row[x])) row[x] &= T_COMP;
                if (x == farX1) break;
            }
            if (y == farY1) break;
        }
    return ok;
}

/* The coast of p closest BY SEA to water tile `from`, as OpenFront picks a
 * boat's departure: the closest coast in a straight line can be across a
 * peninsula, a long way round by sea. The coarse guide numbers the 8x8 cells
 * by sea distance from `from` (out to a little past the straight-line
 * choice), then the coast whose sea tile has the fewest cells to go wins,
 * ties going to the one nearer in a straight line. One guide pass and one
 * scan of p's box: cheap at any ocean size. Returns the land tile and sets
 * *water to the sea tile beside it; NO_TILE if p has no coast on that sea. */
/* cells to sail from sea tile (x, y), as the last coarse_bfs numbered them */
static uint8_t sea_cells(uint16_t x, uint16_t y)
{
    uint16_t c = cell_of(x, y);
    uint8_t k = cSplit[c];
    return k ? splitDist[k - 1][pool_at(c, (uint8_t)x & 7, (uint8_t)y & 7)] : cDist[c];
}

tile_t own_shore_by_sea(uint8_t p, tile_t from, tile_t *water)
{
    static const int8_t ddx[4] = {1, -1, 0, 0}, ddy[4] = {0, 0, 1, -1};
    const player_t *pl = &g_pl[p];
    uint8_t comp = g_map[from] & T_COMP, bestD = 255;
    uint16_t x, y, fy, fx, bestM = 0xFFFF;
    tile_t best = NO_TILE, near;
    if (IS_LAND(g_map[from]) || !comp || !pl->tiles) return NO_TILE;
    near = nearest_own_shore(p, from, comp);
    if (near == NO_TILE) return NO_TILE;
    *water = water_next_to(near, comp);
    path_ready();
    if (!cOn) return near;
    coarse_bfs(*water, from, comp);
    fy = ty(from);
    fx = (uint16_t)(from - g_rowStart[fy]);
    for (y = pl->miny;; y++) {
        const uint8_t *row = g_map + g_rowStart[y];
        for (x = pl->minx;; x++) {
            const uint8_t *q = row + x;
            if (IS_LAND(*q) && (*q & T_OWNER) == p) {
                uint8_t i;
                for (i = 0; i < 4; i++) {
                    uint16_t wx, wy, m;
                    uint8_t d;
                    if ((q[g_dirofs[i]] & (T_TERR | T_COMP)) != comp) continue;
                    wx = (uint16_t)(x + ddx[i]);
                    wy = (uint16_t)(y + ddy[i]);
                    d = sea_cells(wx, wy);
                    if (d > bestD) continue;
                    m = (uint16_t)((wx > fx ? wx - fx : fx - wx) + (wy > fy ? wy - fy : fy - wy));
                    if (d == bestD && m >= bestM) continue;
                    bestD = d;
                    bestM = m;
                    best = (tile_t)(q - g_map);
                    *water = (tile_t)(best + g_dirofs[i]);
                }
            }
            if (x == pl->maxx) break;
        }
        if (y == pl->maxy) break;
    }
    if (best == NO_TILE || bestD == 255) {
        *water = water_next_to(near, comp);
        return near;
    }
    return best;
}

/* a short hop is tried with the plain search and a small budget first; long
 * routes, or hops around a headland, use the coarse guide */
#define SHORT_HOP 40
#define SHORT_BUDGET 500
#define LONG_BUDGET 30000

static bool route(tile_t from, tile_t to, uint8_t slot)
{
    uint8_t a = g_map[from], b = g_map[to];
    if (IS_LAND(a) || IS_LAND(b) || !(a & T_COMP) || (a & T_COMP) != (b & T_COMP)) return false;
    path_ready();
    cUse = false;
    if (mdist(from, to) < SHORT_HOP && search(from, to, slot, SHORT_BUDGET)) return true;
    cUse = cOn;
    if (cUse) coarse_bfs(from, to, a & T_COMP);
    return search(from, to, slot, LONG_BUDGET);
}

uint8_t path_find(tile_t from, tile_t to)
{
    uint8_t i, slot = 0xFF, oldest = 0;
    bool ok;
#ifdef PATH_STATS
    unsigned long p0 = g_pathPops;
#endif
    pathClock++;
    for (i = 0; i < MAX_PATHS; i++)
        if (pathOk[i] == 1 && pathFrom[i] == from && pathTo[i] == to) {
            pathRefs[i]++;
            pathAge[i] = pathClock;
            return i;
        }
    /* free slot: empty first, then the least recently used unreferenced one */
    for (i = 0; i < MAX_PATHS; i++) {
        if (pathRefs[i]) continue;
        if (!pathOk[i]) {
            slot = i;
            break;
        }
        if (slot == 0xFF || (uint8_t)(pathClock - pathAge[i]) > oldest) {
            oldest = (uint8_t)(pathClock - pathAge[i]);
            slot = i;
        }
    }
    if (slot == 0xFF) return 0xFF;
    pathOk[slot] = 0;
    PROF(12, ok = route(from, to, slot));
#ifdef PATH_STATS
    g_pathCalls++;
    if (g_pathPops - p0 > g_pathMax) g_pathMax = g_pathPops - p0;
    if (!ok) g_pathFails++;
#endif
    if (!ok) return 0xFF;
    pathOk[slot] = (uint8_t)(pathlen[slot] == PATH_STEPS ? 2 : 1);
    pathFrom[slot] = from;
    pathTo[slot] = to;
    pathRefs[slot] = 1;
    pathAge[slot] = pathClock;
    return slot;
}

#ifdef CF_SELFTEST
/* Self-test build: the assembly of src/asm/map*.s against the C twins, on the
 * live game (conquests of random tiles, every player's scan, coarse guides
 * and route searches between random sea tiles), each run from the same state
 * and the state put back after. Returns the number of mismatches. Scratch:
 * the route search's heap and vlist (free outside a search). */
static uint24_t st_hash(const void *v, uint24_t n, uint24_t h)
{
    const uint8_t *p = v;
    while (n--) h = h * 33 + *p++;
    return h;
}

static uint24_t st_conq(tile_t t, uint8_t p, bool useC)
{
    if (useC)
        conquer_c(t, p);
    else
        conquer(t, p);
    return st_hash(g_pl, sizeof g_pl, st_hash(coast, sizeof coast, g_map[t] + g_landLeft * 3u + g_mapClean));
}

static tile_t st_sea(uint8_t comp)
{
    uint8_t k;
    for (k = 0; k < 250; k++) {
        tile_t t = rnd_tile(0, g_W, 0, g_H);
        uint8_t b = g_map[t];
        if (!IS_LAND(b) && (b & T_COMP) && (!comp || (b & T_COMP) == comp)) return t;
    }
    return NO_TILE;
}

/* one route search: its result, heap and visited list */
static uint24_t st_search(tile_t from, tile_t to, uint16_t budget, bool useC)
{
    uint24_t h;
    bool ok;
    stUseC = useC;
    ok = search(from, to, 0, budget);
    stUseC = false;
    h = st_hash(&map_srch, sizeof map_srch, ok);
    h = st_hash(pathbuf[0], sizeof pathbuf[0], h + pathlen[0]);
    h = st_hash(hq, (uint24_t)hn * 4, h);
    return st_hash(vlist, (uint24_t)vn * 2, h);
}

/* one coarse guide, from the cPar and splitPar saved in vlist */
static uint24_t st_bfs(tile_t from, tile_t to, uint8_t comp, bool useC)
{
    const uint8_t *save = (const uint8_t *)vlist;
    memcpy(cPar, save, cN);
    memcpy(splitPar, save + CMAX, NSPLIT);
    stUseC = useC;
    coarse_bfs(from, to, comp);
    stUseC = false;
    return st_hash(cDist, cN, st_hash(cPar, cN, st_hash(splitDist, sizeof splitDist,
                                                         st_hash(splitPar, sizeof splitPar, stStates))));
}

uint16_t map_selftest(void)
{
    uint8_t st[4], *pl0 = (uint8_t *)hq, *co0 = (uint8_t *)vlist, pbuf[sizeof pathbuf[0]], p;
    uint16_t bad = 0, k, plen = pathlen[0];
    rnd_get(st);
    memcpy(pbuf, pathbuf[0], sizeof pbuf);
    path_ready();
    /* conquests: g_pl saved in hq and hk (8 KB), the coast counts in vlist */
    memcpy(pl0, g_pl, sizeof g_pl);
    memcpy(co0, coast, sizeof coast);
    for (k = 0; k < 300; k++) {
        tile_t t = k < 40 ? rnd_tile(0, g_W, 0, g_H) : random_land();
        uint8_t b, old, mc = g_mapClean;
        uint16_t ll = g_landLeft;
        uint24_t h1, h2;
        if (t == NO_TILE) continue;
        b = g_map[t];
        old = b & T_OWNER;
        p = (uint8_t)(1 + rnd_n(g_nplayers));
        if (k & 7) {
            /* mostly a neighbour's tile, as attacks take them */
            tile_t n = (tile_t)(t + g_dirofs[k & 3]);
            if (IS_LAND(b) && IS_LAND(g_map[n]) && (g_map[n] & T_OWNER)) p = g_map[n] & T_OWNER;
        }
        if (IS_LAND(b) && (has_struct(t) || (old && old != p && g_pl[old].tiles < 2))) continue;
        h1 = st_conq(t, p, true);
        memcpy(g_pl, pl0, sizeof g_pl);
        memcpy(coast, co0, sizeof coast);
        g_map[t] = b;
        g_landLeft = ll;
        g_mapClean = mc;
        h2 = st_conq(t, p, false);
        if (h1 != h2) bad++;
        memcpy(g_pl, pl0, sizeof g_pl);
        memcpy(coast, co0, sizeof coast);
        g_map[t] = b;
        g_landLeft = ll;
        g_mapClean = mc;
    }
    /* scans of every player (results compared, then the player put back) */
    for (p = 1; p <= g_nplayers; p++) {
        player_t a;
        if (!g_pl[p].tiles) continue;
        scan_player_c(p);
        a = g_pl[p];
        memcpy(&g_pl[p], pl0 + p * sizeof(player_t), sizeof(player_t));
        scan_player(p);
        if (memcmp(&a, &g_pl[p], sizeof a)) bad++;
        memcpy(&g_pl[p], pl0 + p * sizeof(player_t), sizeof(player_t));
    }
    /* coarse guides and route searches between random sea tiles */
    if (cOn)
        for (k = 0; k < 60; k++) {
            tile_t from = st_sea(0), to;
            uint8_t comp;
            uint16_t budget = k % 3 == 0 ? 500 : k % 3 == 1 ? 30000 : (uint16_t)(1 + rnd_n(400));
            uint24_t h1, h2, m0 = 0;
            if (from == NO_TILE) continue;
            comp = g_map[from] & T_COMP;
            to = (k & 1) ? random_water_near(from, 30, comp) : st_sea(comp);
            if (to == NO_TILE) continue;
            memcpy(vlist, cPar, cN);
            memcpy((uint8_t *)vlist + CMAX, splitPar, NSPLIT);
            h1 = st_bfs(from, to, comp, true);
            h2 = st_bfs(from, to, comp, false);
            if (h1 != h2) bad++;
            if (k < 6) m0 = st_hash(g_map, (uint24_t)g_W * g_H, 0);
            cUse = (k & 2) != 0;
            h1 = st_search(from, to, budget, true);
            h2 = st_search(from, to, budget, false);
            cUse = false;
            if (h1 != h2) bad++;
            if (k < 6 && m0 != st_hash(g_map, (uint24_t)g_W * g_H, 0)) bad++;
        }
    memcpy(pathbuf[0], pbuf, sizeof pbuf);
    pathlen[0] = plen;
    rnd_set(st);
    return bad;
}
#endif
