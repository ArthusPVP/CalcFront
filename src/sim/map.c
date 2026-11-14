/* map storage, ownership changes, shoreline queries, water pathfinding, lines */
#include "sim.h"

uint16_t g_W, g_H;
uint16_t g_rowStart[MAP_MAX_H + 1];
uint16_t g_rowOfHi[256];
int16_t g_dirofs[4];
map_info_t g_mi;
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

bool map_load_tiles(uint8_t idx, uint8_t var)
{
    uint16_t size;
    const uint8_t *d;
    if (!map_load_info(idx, var)) return false;
    d = plat_map_open(idx, &size);
    plat_map_tiles(d + g_mi.off, (uint16_t)(g_mi.w * g_mi.h), g_mi.comp);
    memset(g_stbits, 0, 8192);
    memset(g_railbits, 0, sizeof g_mem->railbits);
    /* count the land from the tiles themselves: the share of the map shown to
     * the player and the win check must never include water */
    {
        const uint8_t *p = g_map, *end = g_map + (uint16_t)(g_mi.w * g_mi.h);
        uint16_t n = 0;
        for (; p < end; p++)
            if (IS_LAND(*p)) n++;
        g_mi.land = n;
    }
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
static uint16_t coast[MAXP][8];

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

/* recount every coast (new game or loaded game) */
void coast_rebuild(void)
{
    uint16_t t, n = (uint16_t)(g_W * g_H);
    memset(coast, 0, sizeof coast);
    for (t = 0; t < n; t++) {
        uint8_t b = g_map[t];
        if (IS_LAND(b) && (b & T_OWNER)) coast_move(t, 0, b & T_OWNER);
    }
}

void conquer(tile_t t, uint8_t p)
{
    uint8_t b = g_map[t], old = b & T_OWNER, i;
    uint16_t x, y;
    player_t *pl = &g_pl[p];
    if (old == p || !IS_LAND(b)) return;
    if (old) g_pl[old].tiles--;
    if (b & T_FALL) g_landLeft++;
    coast_move(t, old, p);
    g_map[t] = (uint8_t)((b & T_TERR) | p);
    pl->tiles++;
    y = ty(t);
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

/* tile destroyed by a nuke: nobody owns it, fallout */
void relinquish(tile_t t)
{
    uint8_t b = g_map[t], old = b & T_OWNER;
    if (!IS_LAND(b)) return;
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

void scan_player(uint8_t p)
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
#define hq (g_mem->hq)
#define hk (g_mem->hk)
#define vlist (g_mem->vlist)
#ifdef PATH_STATS
unsigned long g_pathPops, g_pathCalls, g_pathFails, g_pathMax, g_pathBfs, g_pathStates, g_pathOverflow;
#endif
static uint16_t hn;
static uint16_t vn;
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
static uint8_t cCross[CMAX];  /* bits 0-2 water body across the right border, 4-6 bottom */
static uint8_t cSplit[CMAX];  /* 0 (one pool at most), or 1 + index into split* */
static uint8_t cDist[CMAX];   /* cells left from the cell's pool, 255 = unknown */
static uint8_t cPar[CMAX];    /* side toward the goal */
static uint8_t splitLab[NSPLIT][16];  /* pool (0-3) of each tile, 2 bits per tile */
static uint8_t splitSides[NSPLIT][4]; /* sides each pool crosses */
static uint8_t splitDist[NSPLIT][4], splitPar[NSPLIT];
static uint8_t nSplit;
static uint16_t cRow[MAP_MAX_H / CS + 1];
static uint8_t cW, cH, gPool;
static uint16_t cN, cGoal;
static bool cOn;  /* the grid fits this map */
static bool cUse; /* the running search is guided */

static const uint8_t sideBit[4] = {1, 2, 4, 8};
static const int8_t sideDx[4] = {1, -1, 0, 0}, sideDy[4] = {0, 0, 1, -1};

/* Flood the water of a cell (top-left x0, y0, size w x h) from local tile s
 * (ly * 8 + lx), setting lab[] to id. Returns the sides where it crosses into
 * the next cell. The map frame is never navigable, so neighbours exist. */
__attribute__((noinline)) static uint8_t cell_fill(uint16_t x0, uint16_t y0, uint8_t w, uint8_t h,
                                                   uint8_t s, uint8_t *lab, uint8_t id)
{
    uint8_t stk[64], n = 0, sides = 0;
    lab[s] = id;
    stk[n++] = s;
    while (n) {
        uint8_t i = stk[--n], lx = i & 7, ly = i >> 3, k;
        tile_t t = TILE(x0 + lx, y0 + ly);
        uint8_t comp = g_map[t] & T_COMP;
        for (k = 0; k < 4; k++) {
            int8_t nx = (int8_t)(lx + sideDx[k]), ny = (int8_t)(ly + sideDy[k]);
            uint8_t nb = g_map[(tile_t)(t + g_dirofs[k])];
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

static uint8_t cell_size(uint16_t org, uint16_t lim) { return lim - org < CS ? (uint8_t)(lim - org) : CS; }

void path_prepare(void)
{
    uint8_t cx, cy;
    uint16_t c = 0;
    cW = (uint8_t)((g_W + CS - 1) / CS);
    cH = (uint8_t)((g_H + CS - 1) / CS);
    cN = (uint16_t)cW * cH;
    cOn = cN <= CMAX;
    nSplit = 0;
    if (!cOn) return;
    for (cy = 0; cy < cH; cy++) {
        uint16_t y0 = (uint16_t)cy * CS;
        uint8_t h = cell_size(y0, g_H);
        cRow[cy] = c;
        for (cx = 0; cx < cW; cx++, c++) {
            uint16_t x0 = (uint16_t)cx * CS;
            uint8_t w = cell_size(x0, g_W), lab[64], i, cross = 0, pools = 0;
            uint8_t sides[4] = {0, 0, 0, 0};
            memset(lab, 0, sizeof lab);
            for (i = 0; i < 64; i++) {
                uint8_t lx = i & 7, ly = i >> 3, b, sd;
                if (lx >= w || ly >= h || lab[i]) continue;
                b = g_map[TILE(x0 + lx, y0 + ly)];
                if ((b & T_TERR) || !(b & T_COMP)) continue;
                if (pools < 4) pools++; /* a 5th pool shares the 4th one's slot */
                sd = cell_fill(x0, y0, w, h, i, lab, pools);
                sides[pools - 1] |= sd;
                if ((sd & 1) && !(cross & 0x07)) cross |= b & T_COMP;
                if ((sd & 4) && !(cross & 0x70)) cross |= (uint8_t)((b & T_COMP) << 4);
            }
            cCross[c] = cross;
            cSplit[c] = 0;
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
}

/* pool of local tile i (ly * 8 + lx) of cell c */
FORCE_INLINE uint8_t pool_at(uint16_t c, uint8_t i)
{
    uint8_t k = cSplit[c], b;
    if (!k) return 0;
    b = splitLab[k - 1][i >> 2];
    switch (i & 3) { /* constant shifts: a variable one is a library loop */
    case 0: return b & 3;
    case 1: return (b >> 2) & 3;
    case 2: return (b >> 4) & 3;
    default: return b >> 6;
    }
}

FORCE_INLINE uint8_t node_dist(uint16_t c, uint8_t pool)
{
    uint8_t k = cSplit[c];
    return k ? splitDist[k - 1][pool] : cDist[c];
}

FORCE_INLINE uint8_t node_par(uint16_t c, uint8_t pool)
{
    uint8_t k = cSplit[c];
    return k ? (splitPar[k - 1] >> (pool * 2)) & 3 : cPar[c];
}

static void node_set(uint16_t c, uint8_t pool, uint8_t d, uint8_t side)
{
    uint8_t k = cSplit[c];
    if (k) {
        uint8_t sh = (uint8_t)(pool * 2);
        splitDist[k - 1][pool] = d;
        splitPar[k - 1] = (uint8_t)((splitPar[k - 1] & ~(3 << sh)) | (side << sh));
    } else {
        cDist[c] = d;
        cPar[c] = side;
    }
}

/* water body crossing side s of cell c (0 = none) */
FORCE_INLINE uint8_t cell_cross(uint16_t c, uint8_t s)
{
    switch (s) {
    case 0: return cCross[c] & 7;
    case 1: return c ? cCross[c - 1] & 7 : 0; /* row starts: the previous cell is at the map edge */
    case 2: return cCross[c] >> 4;
    default: return c >= cW ? cCross[c - cW] >> 4 : 0;
    }
}

FORCE_INLINE uint16_t cell_next(uint16_t c, uint8_t s)
{
    switch (s) {
    case 0: return c + 1;
    case 1: return c - 1;
    case 2: return c + cW;
    default: return c - cW;
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
static uint8_t border_pools(uint16_t c, uint8_t p, uint8_t a, uint16_t nc)
{
    uint8_t cy = (uint8_t)(c / cW), cx = (uint8_t)(c - (uint16_t)cy * cW), w, h, n, i, pools = 0;
    uint16_t x0 = (uint16_t)cx * CS, y0 = (uint16_t)cy * CS;
    w = cell_size(x0, g_W);
    h = cell_size(y0, g_H);
    n = a < 2 ? h : w;
    for (i = 0; i < n; i++) {
        uint8_t lx, ly, nlx, nly, bu;
        tile_t u;
        switch (a) {
        case 0: lx = (uint8_t)(w - 1); ly = i; nlx = 0; nly = i; break;
        case 1: lx = 0; ly = i; nlx = 7; nly = i; break;
        case 2: lx = i; ly = (uint8_t)(h - 1); nlx = i; nly = 0; break;
        default: lx = i; ly = 0; nlx = i; nly = 7;
        }
        u = TILE(x0 + lx, y0 + ly);
        bu = g_map[u] & (T_TERR | T_COMP);
        if (!bu || (bu & T_TERR) || bu != (g_map[(tile_t)(u + g_dirofs[a])] & (T_TERR | T_COMP))) continue;
        if (pool_at(c, (uint8_t)((ly << 3) | lx)) != p) continue;
        pools |= sideBit[pool_at(nc, (uint8_t)((nly << 3) | nlx))];
    }
    return pools;
}

/* number the cells from the goal outward, until a few layers past the start */
static void coarse_bfs(tile_t from, tile_t to, uint8_t comp)
{
    uint16_t *q = hq; /* hq, hk and vlist follow each other: room for 3 * 2048 */
    uint16_t qh = 0, qt = 0, gy = ty(to), gx = (uint16_t)(to - g_rowStart[gy]);
    uint16_t sy = ty(from), sx = (uint16_t)(from - g_rowStart[sy]), sc = cell_of(sx, sy);
    uint8_t stop = 255, sPool = pool_at(sc, (uint8_t)(((sy & 7) << 3) | (sx & 7)));
    cGoal = cell_of(gx, gy);
    gPool = pool_at(cGoal, (uint8_t)(((gy & 7) << 3) | (gx & 7)));
    memset(cDist, 255, cN);
    memset(splitDist, 255, (size_t)nSplit * 4);
    node_set(cGoal, gPool, 0, 0);
    q[qt++] = (uint16_t)(cGoal * 4 + gPool);
#ifdef PATH_STATS
    g_pathBfs++;
#endif
    while (qh < qt) {
        u16b_t st, cc;
        uint16_t c;
        uint8_t p, d, k, a, sides = 0;
        st.v = q[qh++];
#ifdef PATH_STATS
        g_pathStates++;
#endif
        p = st.b[0] & 3;
        cc.b[0] = (uint8_t)((st.b[0] >> 2) | (uint8_t)(st.b[1] << 6)); /* c = st >> 2 */
        cc.b[1] = (uint8_t)(st.b[1] >> 2);
        c = cc.v;
        d = node_dist(c, p);
        k = cSplit[c];
        if (c == sc && p == sPool && stop == 255) stop = (uint8_t)(d > 250 ? 253 : d + 3);
        if (d >= stop) break;
        if (k)
            sides = splitSides[k - 1][p];
        else
            for (a = 0; a < 4; a++)
                if (cell_cross(c, a) == comp) sides |= sideBit[a];
        for (a = 0; a < 4; a++) {
            uint16_t nc;
            uint8_t pools, np, kn;
            if (!(sides & sideBit[a])) continue;
            nc = cell_next(c, a);
            kn = cSplit[nc];
            if (!kn) {
                /* one pool there, and pool p crosses side a: they touch */
                if (cDist[nc] == 255) {
                    cDist[nc] = d < 254 ? (uint8_t)(d + 1) : 254;
                    cPar[nc] = a ^ 1;
                    q[qt++] = (uint16_t)(nc * 4);
                }
                continue;
            } else if (!k) {
                /* we are one pool: the neighbour's pools that cross back to us */
                uint8_t q, back = sideBit[a ^ 1];
                pools = 0;
                for (q = 0; q < 4; q++)
                    if (splitSides[kn - 1][q] & back) pools |= sideBit[q];
            } else
                pools = border_pools(c, p, a, nc); /* split on both sides: look */
            for (np = 0; np < 4; np++) {
                if (!(pools & sideBit[np]) || node_dist(nc, np) != 255) continue;
                node_set(nc, np, d < 254 ? (uint8_t)(d + 1) : 254, a ^ 1);
                q[qt++] = (uint16_t)(nc * 4 + np);
            }
        }
    }
}

/* search key of tile (x, y): Manhattan distance to the goal, or with the
 * guide, 16 per cell left plus the distance to the cell side toward the goal */
static uint16_t key_of(uint16_t x, uint16_t y, uint16_t gx, uint16_t gy)
{
    uint16_t m = (uint16_t)((x > gx ? x - gx : gx - x) + (y > gy ? y - gy : gy - y));
    if (cUse) {
        uint16_t c = cell_of(x, y);
        uint8_t lx = (uint8_t)x & 7, ly = (uint8_t)y & 7, pool = pool_at(c, (uint8_t)((ly << 3) | lx)), d, b;
        if (c == cGoal && pool == gPool) return m;
        d = node_dist(c, pool);
        if (d == 255) return (uint16_t)(0xF000 | (m > 0xFFF ? 0xFFF : m));
        switch (node_par(c, pool)) {
        case 0: b = (uint8_t)(7 - lx); break;
        case 1: b = lx; break;
        case 2: b = (uint8_t)(7 - ly); break;
        default: b = ly;
        }
        return (uint16_t)(d * 16 + b);
    }
    return m;
}

/* (i - 1) / 2 for a heap index, with byte shifts */
FORCE_INLINE uint16_t parent(uint16_t i)
{
    u16b_t u, r;
    u.v = (uint16_t)(i - 1);
    r.b[0] = (uint8_t)((u.b[0] >> 1) | (u.b[1] << 7));
    r.b[1] = (uint8_t)(u.b[1] >> 1);
    return r.v;
}

static void heap_push(tile_t t, uint16_t k)
{
    uint16_t i = hn++;
    while (i) {
        uint16_t p = parent(i);
        if (hk[p] <= k) break;
        hq[i] = hq[p];
        hk[i] = hk[p];
        i = p;
    }
    hq[i] = t;
    hk[i] = k;
}

static tile_t heap_pop(void)
{
    tile_t top = hq[0], t = hq[--hn];
    uint16_t k = hk[hn], i = 0;
    for (;;) {
        uint16_t c = (uint16_t)(2 * i + 1);
        if (c >= hn) break;
        if (c + 1 < hn && hk[c + 1] < hk[c]) c++;
        if (hk[c] >= k) break;
        hq[i] = hq[c];
        hk[i] = hk[c];
        i = c;
    }
    hq[i] = t;
    hk[i] = k;
    return top;
}

/* Greedy best-first search over one water body, expanding at most `budget`
 * tiles. Each visited tile remembers the direction back to its parent. The
 * map frame is water body 0, so the search never leaves the map. */
static bool search(tile_t from, tile_t to, uint8_t slot, uint16_t budget)
{
    uint16_t n = 0, gx, gy, x0 = 0xFFFF, x1 = 0, y0 = 0xFFFF, y1 = 0, x, y, k;
    uint8_t comp = g_map[from] & T_COMP, i;
    bool ok = false, overflow = false;
    tile_t t;

    pathlen[slot] = 0;
    if (from == to) return true;

    gy = ty(to);
    gx = (uint16_t)(to - g_rowStart[gy]);
    hn = vn = 0;
    g_map[from] |= W_VIS;
    vlist[vn++] = from;
    heap_push(from, 0);
    while (hn) {
        uint16_t px, py;
        t = heap_pop();
        if (t == to) {
            ok = true;
            break;
        }
        if (!budget--) break;
#ifdef PATH_STATS
        g_pathPops++;
#endif
        py = ty(t);
        px = (uint16_t)(t - g_rowStart[py]);
        for (i = 0; i < 4; i++) {
            static const uint8_t backBits[4] = {1 << W_DIRSH, 0, 3 << W_DIRSH, 2 << W_DIRSH};
            tile_t nt = (tile_t)(t + g_dirofs[i]);
            uint8_t nb = g_map[nt];
            if ((nb & (T_TERR | W_VIS)) || (nb & T_COMP) != comp) continue;
            if (hn >= HEAPN) break; /* heap full: leave it to another neighbour */
            g_map[nt] = (uint8_t)(nb | W_VIS | backBits[i]); /* direction back: i ^ 1 */
            x = px;
            y = py;
            switch (i) {
            case 0: x++; break;
            case 1: x--; break;
            case 2: y++; break;
            default: y--;
            }
            if (vn < VMAX)
                vlist[vn++] = nt;
            else {
                overflow = true;
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
            heap_push(nt, key_of(x, y, gx, gy));
        }
    }
    if (ok) {
        /* count the steps walking back from the goal, then store them forward */
        for (t = to; t != from; n++) t = (tile_t)(t + g_dirofs[(g_map[t] >> W_DIRSH) & 3]);
        memset(pathbuf[slot], 0, PATH_STEPS / 4);
        for (t = to, k = n; t != from;) {
            uint8_t back = (g_map[t] >> W_DIRSH) & 3;
            k--;
            if (k < PATH_STEPS) pathbuf[slot][k >> 2] |= (uint8_t)((back ^ 1) << ((k & 3) * 2));
            t = (tile_t)(t + g_dirofs[back]);
        }
        pathlen[slot] = n > PATH_STEPS ? PATH_STEPS : n;
    }
    /* clear scratch bits */
    for (k = 0; k < vn; k++) g_map[vlist[k]] &= T_COMP;
#ifdef PATH_STATS
    if (overflow) g_pathOverflow++;
#endif
    if (overflow)
        for (y = y0;; y++) {
            uint8_t *row = g_map + g_rowStart[y];
            for (x = x0;; x++) {
                if (!IS_LAND(row[x])) row[x] &= T_COMP;
                if (x == x1) break;
            }
            if (y == y1) break;
        }
    return ok;
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
