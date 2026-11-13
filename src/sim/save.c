/* saving and resuming a game
 *
 * The front end writes its own header (map file name, view), then this
 * stream: magic, core values, players, active attacks (without their sample
 * of front tiles, rescanned on the next tick), units, the owner and fallout
 * of every land tile (run-length coded), and the magic again. Water and
 * terrain come from the map file. Rebuilt on load: the structure bitmap,
 * railways, ship routes and the per-tick caches. */
#include <stddef.h>
#include "sim.h"

static const char magic[4] = {'C', 'F', 'G', '1'};

typedef struct {
    game_cfg_t cfg;
    uint32_t tick;
    uint16_t w, h, land, landLeft;
    uint8_t S, L16, winner, mirvs, nplayers, unHi, nAtk;
    uint8_t rng[4];
} core_t;

#define ATK_LEN offsetof(attack_t, ring)

/* Land tiles, run-length coded, in blocks [n][n bytes] ending with [0].
 * A byte below 0x40 is one tile with that value; a byte b >= 0x40 is a run of
 * b - 0x40 + 2 tiles of the value in the next byte. Value: owner in bits 0-4,
 * fallout in bit 5. */
#define BLK 128
#define RUN_MAX (0xFF - 0x40 + 2)
static uint8_t blk[BLK + 1]; /* blk[0] = length of the block */
static uint8_t bn, bpos;

static bool put(uint8_t b)
{
    blk[1 + bn++] = b;
    if (bn < BLK) return true;
    blk[0] = BLK;
    bn = 0;
    return plat_save_write(blk, BLK + 1);
}

static bool put_run(uint8_t v, uint16_t n)
{
    while (n) {
        uint8_t k = n > RUN_MAX ? RUN_MAX : (uint8_t)n;
        if (k == 1) {
            if (!put(v)) return false;
        } else if (!put((uint8_t)(0x40 + k - 2)) || !put(v))
            return false;
        n -= k;
    }
    return true;
}

static uint8_t tile_val(uint8_t b) { return (uint8_t)((b & T_OWNER) | ((b & T_FALL) ? 0x20 : 0)); }

bool game_save(void)
{
    core_t c;
    uint8_t i;
    uint16_t t, n = 0, total = (uint16_t)(g_W * g_H);
    uint8_t v = 0xFF;

    memset(&c, 0, sizeof c);
    c.cfg = g_cfg;
    c.tick = g_tick;
    c.w = g_W;
    c.h = g_H;
    c.land = g_mi.land;
    c.landLeft = g_landLeft;
    c.S = g_S;
    c.L16 = g_L16;
    c.winner = g_winner;
    c.mirvs = g_mirvs;
    c.nplayers = g_nplayers;
    c.unHi = g_unHi;
    for (i = 0; i < MAX_ATTACKS; i++) c.nAtk += g_at[i].active ? 1 : 0;
    rnd_get(c.rng);
    if (!plat_save_write(magic, 4) || !plat_save_write(&c, sizeof c)) return false;
    for (i = 1; i <= g_nplayers; i++)
        if (!plat_save_write(&g_pl[i], PLAYER_BYTES)) return false;
    for (i = 0; i < MAX_ATTACKS; i++)
        if (g_at[i].active && (!plat_save_write(&i, 1) || !plat_save_write(&g_at[i], ATK_LEN)))
            return false;
    for (i = 0; i < g_unHi; i++)
        if (!plat_save_write(&g_un[i], UNIT_BYTES)) return false;

    bn = 0;
    for (t = 0; t < total; t++) {
        uint8_t b = g_map[t], w;
        if (!IS_LAND(b)) continue;
        w = tile_val(b);
        if (w == v) {
            n++;
            continue;
        }
        if (n && !put_run(v, n)) return false;
        v = w;
        n = 1;
    }
    if (n && !put_run(v, n)) return false;
    if (bn) {
        blk[0] = bn;
        if (!plat_save_write(blk, (uint16_t)(bn + 1))) return false;
    }
    blk[0] = 0;
    return plat_save_write(blk, 1) && plat_save_write(magic, 4);
}

/* next byte of the tile stream, -1 at the end or on a read error */
static int16_t get(void)
{
    if (bpos == bn) {
        if (!plat_save_read(blk, 1) || !blk[0] || !plat_save_read(blk + 1, blk[0])) return -1;
        bn = blk[0];
        bpos = 0;
    }
    return blk[1 + bpos++];
}

bool game_load(uint8_t mapIdx)
{
    core_t c;
    char m[4];
    uint8_t i;
    uint16_t t, run = 0, total;
    int16_t v = 0;

    if (!plat_save_read(m, 4) || memcmp(m, magic, 4) || !plat_save_read(&c, sizeof c)) return false;
    if (!c.nplayers || c.nplayers >= MAXP || c.unHi > MAX_UNITS || c.nAtk > MAX_ATTACKS) return false;
    if (!map_load_tiles(mapIdx, c.cfg.size)) return false;
    if (g_W != c.w || g_H != c.h || g_mi.land != c.land) return false; /* not the same map */

    g_cfg = c.cfg;
    g_cfg.map = mapIdx;
    g_tick = c.tick;
    g_landLeft = c.landLeft;
    g_S = c.S;
    g_L16 = c.L16;
    g_winner = c.winner;
    g_mirvs = c.mirvs;
    g_nplayers = c.nplayers;
    rnd_set(c.rng);

    memset(g_pl, 0, sizeof g_pl);
    for (i = 1; i <= g_nplayers; i++)
        if (!plat_save_read(&g_pl[i], PLAYER_BYTES)) return false;
    memset(g_at, 0, sizeof g_at);
    for (i = 0; i < c.nAtk; i++) {
        uint8_t k;
        if (!plat_save_read(&k, 1) || k >= MAX_ATTACKS || !plat_save_read(&g_at[k], ATK_LEN)) return false;
        g_at[k].head = g_at[k].count = 0; /* rescan the front on the next tick */
        g_at[k].rescan = 1;
    }
    memset(g_un, 0, sizeof g_un);
    g_unHi = c.unHi;
    for (i = 0; i < g_unHi; i++) {
        unit_t *u = &g_un[i];
        if (!plat_save_read(u, UNIT_BYTES)) return false;
        if (u->type >= U_TYPES || u->owner > g_nplayers) return false;
        u->path = 0xFF; /* routes are searched again */
        if (IS_SHIP(u->type)) u->timer2 = 0;
        if (IS_STRUCT(u->type)) st_set(u->pos, true);
    }

    bn = bpos = 0;
    total = (uint16_t)(g_W * g_H);
    for (t = 0; t < total; t++) {
        uint8_t b = g_map[t];
        if (!IS_LAND(b)) continue;
        if (!run) {
            v = get();
            if (v < 0) return false;
            run = 1;
            if (v >= 0x40) {
                run = (uint16_t)(v - 0x40 + 2);
                v = get();
                if (v < 0 || v >= 0x40) return false;
            }
            if ((v & T_OWNER) > g_nplayers) return false;
        }
        g_map[t] = (uint8_t)((b & T_TERR) | (v & T_OWNER) | ((v & 0x20) ? T_FALL : 0));
        run--;
    }
    if (run || bpos != bn || !plat_save_read(blk, 1) || blk[0] || !plat_save_read(m, 4) ||
        memcmp(m, magic, 4))
        return false;

    path_reset();
    path_prepare();
    coast_rebuild();
    rail_reset();
    rail_rebuild();
    units_refresh();
    return true;
}
