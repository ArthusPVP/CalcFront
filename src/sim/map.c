/* map storage, ownership changes, shoreline queries, water pathfinding, lines */
#include "sim.h"

uint16_t g_W, g_H;
uint16_t g_rowStart[MAP_MAX_H + 1];
uint16_t g_rowOfHi[256];
int16_t g_dirofs[4];
map_info_t g_mi;
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

void conquer(tile_t t, uint8_t p)
{
    uint8_t b = g_map[t], old = b & T_OWNER, i;
    uint16_t x, y;
    player_t *pl = &g_pl[p];
    if (old == p || !IS_LAND(b)) return;
    if (old) g_pl[old].tiles--;
    if (b & T_FALL) g_landLeft++;
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
                pl->nbrs |= BIT(o);
                g_pl[o].nbrs |= BIT(p);
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
                            mask |= BIT(o);
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
        tile_t t = TILE(rnd_n(g_W), rnd_n(g_H));
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

tile_t nearest_own_shore(uint8_t p, tile_t to, uint8_t comp)
{
    player_t *pl = &g_pl[p];
    uint16_t x, y, tox = tx(to), toy = ty(to);
    tile_t best = NO_TILE;
    uint16_t bd = 0xFFFF;
    if (!pl->tiles) return NO_TILE;
    for (y = pl->miny;; y++) {
        for (x = pl->minx;; x++) {
            tile_t t = TILE(x, y);
            uint8_t b = g_map[t];
            if ((b & T_OWNER) == p && IS_LAND(b) && water_next_to(t, comp) != NO_TILE) {
                uint16_t dx = x > tox ? x - tox : tox - x;
                uint16_t dy = y > toy ? y - toy : toy - y;
                uint16_t d = dx + dy;
                if (d < bd) {
                    bd = d;
                    best = t;
                }
            }
            if (x == pl->maxx) break;
        }
        if (y == pl->maxy) break;
    }
    return best;
}

/* -------------------------------------------------------------- lines --- */
uint16_t line_len(tile_t a, tile_t b)
{
    int16_t dx = (int16_t)tx(b) - (int16_t)tx(a), dy = (int16_t)ty(b) - (int16_t)ty(a);
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return (uint16_t)(dx > dy ? dx : dy);
}

tile_t line_point(tile_t a, tile_t b, uint16_t k, uint16_t n)
{
    int16_t ax = (int16_t)tx(a), ay = (int16_t)ty(a);
    int16_t dx = (int16_t)tx(b) - ax, dy = (int16_t)ty(b) - ay;
    int32_t x, y;
    if (!n) return a;
    x = ax + ((int32_t)dx * k * 2 + (dx >= 0 ? n : -(int32_t)n)) / (2 * (int32_t)n);
    y = ay + ((int32_t)dy * k * 2 + (dy >= 0 ? n : -(int32_t)n)) / (2 * (int32_t)n);
    return TILE((uint16_t)x, (uint16_t)y);
}

/* ------------------------------------------------------------ pathing --- */
/* Paths are shared: ships sailing between the same two tiles reuse one
 * route, and finished routes stay cached until their slot is needed. */
#define hq (g_mem->hq)
#define hk (g_mem->hk)
#define vlist (g_mem->vlist)
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

static void heap_push(tile_t t, uint16_t k)
{
    uint16_t i = hn++;
    while (i) {
        uint16_t p = (uint16_t)((i - 1) >> 1);
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

/* Greedy best-first search over one water body, always expanding the tile
 * closest to the goal: on open sea it walks almost straight there. Each
 * visited tile remembers the direction back to its parent. The map frame is
 * water body 0, so the search never leaves the map. */
static bool search(tile_t from, tile_t to, uint8_t slot)
{
    uint16_t n = 0, gx, gy, x0 = 0xFFFF, x1 = 0, y0 = 0xFFFF, y1 = 0, x, y, k;
    uint8_t comp = g_map[from] & T_COMP, i;
    bool ok = false, overflow = false;
    tile_t t;

    pathlen[slot] = 0;
    if (!comp || IS_LAND(g_map[from]) || IS_LAND(g_map[to]) || (g_map[to] & T_COMP) != comp)
        return false;
    if (from == to) return true;

    gy = ty(to);
    gx = (uint16_t)(to - g_rowStart[gy]);
    hn = vn = 0;
    g_map[from] |= W_VIS;
    vlist[vn++] = from;
    heap_push(from, 0);
    while (hn) {
        t = heap_pop();
        if (t == to) {
            ok = true;
            break;
        }
        for (i = 0; i < 4; i++) {
            tile_t nt = (tile_t)(t + g_dirofs[i]);
            uint8_t nb = g_map[nt];
            if ((nb & (T_TERR | W_VIS)) || (nb & T_COMP) != comp) continue;
            g_map[nt] = (uint8_t)(nb | W_VIS | ((i ^ 1) << W_DIRSH));
            y = ty(nt);
            x = (uint16_t)(nt - g_rowStart[y]);
            if (vn < VMAX)
                vlist[vn++] = nt;
            else {
                overflow = true;
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
            if (hn < HEAPN)
                heap_push(nt, (uint16_t)((x > gx ? x - gx : gx - x) + (y > gy ? y - gy : gy - y)));
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

uint8_t path_find(tile_t from, tile_t to)
{
    uint8_t i, slot = 0xFF, oldest = 0;
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
    if (!search(from, to, slot)) return 0xFF;
    pathOk[slot] = (uint8_t)(pathlen[slot] == PATH_STEPS ? 2 : 1);
    pathFrom[slot] = from;
    pathTo[slot] = to;
    pathRefs[slot] = 1;
    pathAge[slot] = pathClock;
    return slot;
}
