/* players, game setup, per-tick economy, win check */
#include "sim.h"

player_t g_pl[MAXP];
uint8_t g_nplayers;
game_cfg_t g_cfg;
uint24_t g_tick;
uint8_t g_S, g_L16;
uint16_t g_landLeft;
uint8_t g_winner;
uint8_t g_mirvs;

const uint16_t g_mulQ8[N_MULS] = {128, 256, 384, 512, 768, 1280};

static const uint16_t startTroops[4] = {12500, 18750, 25000, 31250};
/* nation growth by difficulty, 230 243 256 269 in Q8: the low byte (the part
 * above 256 from hard on) */
__attribute__((used)) const uint8_t growLo[4] = {230, 243, 0, 13};

/* The economy's hot paths are hand-written eZ80 on the calculator
 * (src/asm/econ.s): econ_grow() (troop growth and gold, every player every
 * tick) and calc_max() (troop cap, every 16 ticks), with the helpers
 * econ_div8() and econ_mulq8(). The C versions below (suffix _c) are the PC
 * harness's; on the calculator econ_grow_c() also takes the rare cases (a
 * troop multiplier, troops or cap from 2^24), and the self-test
 * (CF_SELFTEST) compares each pair.
 *
 * econ_div8(n, d) = n / ((d >> 8) + 1) for n <= d < 2^24 (it fits 8 bits, as
 * d < 256 * ((d >> 8) + 1)): 8 restoring steps. econ_mulq8(a, g) =
 * (a * g) >> 8 for a < 2^16, from two 8x8 MLTs. */
#if defined(HOST) || defined(CF_NOASM) || defined(CF_SELFTEST)
uint8_t econ_div8_c(uint24_t n, uint24_t d) { return (uint8_t)(n / ((d >> 8) + 1)); }
uint24_t econ_mulq8_c(uint24_t a, uint8_t g) { return (uint24_t)((a * g) >> 8); }
/* the relations to players 1..31: one point toward 0 */
void econ_fade_c(int8_t *r)
{
    uint8_t k;
    for (k = 1; k < MAXP; k++, r++)
        if (*r) *r += *r < 0 ? 1 : -1;
}
#endif
#if defined(HOST) || defined(CF_NOASM)
#define econ_grow econ_grow_c
#define calc_max calc_max_c
#define econ_div8 econ_div8_c
#define econ_mulq8 econ_mulq8_c
#define econ_fade econ_fade_c
#else
void econ_fade(int8_t *r);
void econ_grow(player_t *pl);
void calc_max(player_t *pl);
uint8_t econ_div8(uint24_t n, uint24_t d);
uint24_t econ_mulq8(uint24_t a, uint8_t g);
/* offsets the assembly uses */
_Static_assert(offsetof(player_t, type) == 0 && offsetof(player_t, troops) == 3 &&
                   offsetof(player_t, maxTroops) == 7 && offsetof(player_t, gold) == 11 &&
                   offsetof(player_t, growth) == 15 && offsetof(player_t, tiles) == 20 &&
                   offsetof(player_t, big8) == 47 && offsetof(player_t, cityLv) == 50,
               "player_t offsets used by src/asm/econ.s");
_Static_assert(offsetof(game_cfg_t, difficulty) == 2 && offsetof(game_cfg_t, goldMul) == 5 &&
                   offsetof(game_cfg_t, troopMul) == 6 && PT_NATION == 2 && PT_TRIBE == 3 &&
                   DIFF_HARD == 2,
               "game_cfg_t offsets and constants used by src/asm/econ.s");
#endif

__attribute__((used)) uint32_t gold_mul(uint32_t g)
{
    uint16_t m = g_mulQ8[g_cfg.goldMul];
    return m == 256 ? g : (g >> 8) * m + (((g & 255) * m) >> 8);
}

/* not static: the assembly calls it */
__attribute__((used)) uint32_t troop_mul(uint32_t t)
{
    uint16_t m = g_mulQ8[g_cfg.troopMul];
    return m == 256 ? t : (t >> 8) * m + (((t & 255) * m) >> 8);
}

bool can_trade(uint8_t a, uint8_t b)
{
    return !MASKBIT(g_pl[a].embargo, b) && !MASKBIT(g_pl[b].embargo, a);
}

#define TROOP_CAP 60000000UL

#if defined(HOST) || defined(CF_NOASM) || defined(CF_SELFTEST)
void calc_max_c(player_t *pl)
{
    /* n = tiles * S < 2^22, so log2(n) < 22 * 256 and the Q8 power below is
     * < 2^22: 24-bit math up to the 32-bit sum */
    uint24_t n = (uint24_t)pl->tiles * g_S, e;
    uint16_t lg = fx_log2(n); /* log2(0) = log2(1) = 0 */
    uint32_t m;
    int16_t l;
    /* 2 * (n^0.6 * 1000 + 50000) + cities */
    e = (uint24_t)fx_exp2((int16_t)((((uint24_t)lg * 154) >> 8) + 2048)); /* Q8 */
    /* (e * 125) >> 5 with e = 32a + b: 125a + (125b >> 5), < 2^24 */
    m = 2 * ((uint32_t)((e >> 5) * 125 + ((((uint8_t)e & 31) * 125) >> 5)) + 50000);
    if (pl->cityLv) m += (uint32_t)pl->cityLv * 250000;
    if (pl->type == PT_TRIBE)
        m /= 3;
    else if (pl->type == PT_NATION)
        m = (m >> 2) * (2 + g_cfg.difficulty);
    m = troop_mul(m);
    if (m > TROOP_CAP) m = TROOP_CAP;
    pl->maxTroops = m;

    /* large territory sigmoid: 1 / (1 + (300000/n)^2.5) */
    l = (int16_t)(lg - 4658);
    l = (int16_t)(l * 5 / 2);
    if (l < -2000)
        pl->big8 = 0;
    else if (l > 2000)
        pl->big8 = 255;
    else {
        /* 2^(l/256 + 8) <= 2^15.82 < 65536: x8 * 255 fits 24 bits */
        uint24_t x8 = (uint24_t)fx_exp2((int16_t)(l + 2048));
        pl->big8 = (uint8_t)((x8 * 255) / (256 + x8));
    }
}
#endif

void add_troops(uint8_t p, uint32_t n)
{
    player_t *pl = &g_pl[p];
    pl->troops += n;
    if (pl->troops > TROOP_CAP) pl->troops = TROOP_CAP;
}

void sub_troops(uint8_t p, uint32_t n)
{
    player_t *pl = &g_pl[p];
    pl->troops = pl->troops > n ? pl->troops - n : 0;
}

/* Share of the land in 0.01%. g_landLeft counts land tiles only (water and
 * lakes never), minus fallout, like OpenFront's win check. Instead of a
 * 32-bit division per call, a 16.16 reciprocal is kept and refreshed only
 * when g_landLeft changes (a nuke or a fallout tile taken back). */
static uint16_t pctLand;
static uint32_t pctMul;

uint16_t land_pct100(uint8_t p)
{
    if (g_landLeft != pctLand) {
        pctLand = g_landLeft;
        pctMul = pctLand ? (10000UL << 16) / pctLand : 0;
    }
    /* tiles <= g_landLeft, so tiles * pctMul <= 10000 << 16: no overflow */
    return (uint16_t)(((uint32_t)g_pl[p].tiles * pctMul + 0x8000) >> 16);
}

uint16_t land_pct10(uint8_t p) { return (uint16_t)((land_pct100(p) + 5) / 10); }

/* a fingerprint of the game state, to compare a calculator run with the PC */
uint16_t sim_checksum(void)
{
    uint16_t t, n = (uint16_t)(g_W * g_H);
    uint8_t a = 0, b = 0, p;
    for (t = 0; t < n; t++) {
        a += g_map[t];
        b += a;
    }
    for (p = 1; p <= g_nplayers; p++) {
        a += (uint8_t)g_pl[p].troops ^ (uint8_t)g_pl[p].gold;
        b += a;
    }
    return (uint16_t)(b << 8 | a);
}

uint8_t add_player(uint8_t type, const char *name)
{
    uint8_t p = ++g_nplayers;
    player_t *pl = &g_pl[p];
    memset(pl, 0, sizeof *pl);
    pl->type = type;
    pl->ratio = 20;
    pl->minx = pl->miny = 0xFFFF;
    strncpy(pl->name, name, NATION_NAME_LEN - 1);
    return p;
}

void game_new(const game_cfg_t *cfg, uint32_t seed)
{
    uint16_t s;
    g_cfg = *cfg;
    rnd_seed(seed);
    map_load_tiles(cfg->map, cfg->size);
    memset(g_pl, 0, sizeof g_pl);
    memset(g_at, 0, sizeof g_at);
    memset(g_un, 0, sizeof g_un);
    g_unHi = 0;
    path_reset();
    rail_reset();
    g_nplayers = 0;
    g_tick = 0;
    g_winner = 0;
    g_mirvs = 0;
    g_landLeft = g_mi.land;
    path_prepare_begin(); /* built while the player picks a start (or on first use) */
    coast_rebuild(false);
    s = g_mi.scale16 >> 4;
    if (s < 12) s = 12;
    if (s > 40) s = 40;
    g_S = (uint8_t)s;
    g_L16 = (uint8_t)isqrt32((uint32_t)s * 256);
    add_player(PT_HUMAN, "You");
}

bool can_spawn_at(tile_t t)
{
    int8_t dx, dy;
    uint16_t x = tx(t), y = ty(t);
    if (x < 3 || y < 3 || x + 3 >= g_W || y + 3 >= g_H) return false;
    if (!IS_LAND(g_map[t]) || (g_map[t] & (T_OWNER | T_FALL))) return false;
    for (dy = -2; dy <= 2; dy++)
        for (dx = -2; dx <= 2; dx++) {
            uint8_t b = g_map[TILE(x + dx, y + dy)];
            if (IS_LAND(b) && (b & T_OWNER)) return false;
        }
    return true;
}

bool game_spawn(uint8_t p, tile_t t)
{
    player_t *pl = &g_pl[p];
    int8_t dx, dy;
    uint16_t x = tx(t), y = ty(t);
    if (!can_spawn_at(t)) return false;
    pl->alive = 1;
    for (dy = -1; dy <= 1; dy++)
        for (dx = -1; dx <= 1; dx++) {
            tile_t n = TILE(x + dx, y + dy);
            if (IS_LAND(g_map[n]) && !(g_map[n] & T_OWNER)) conquer(n, p);
        }
    if (pl->type == PT_TRIBE)
        pl->troops = 10000;
    else if (pl->type == PT_NATION)
        pl->troops = startTroops[g_cfg.difficulty];
    else
        pl->troops = 25000;
    pl->troops = troop_mul(pl->troops);
    calc_max(pl);
    ai_init_player(p);
    return true;
}

static tile_t free_spot_near(tile_t t)
{
    uint8_t r, tries;
    uint16_t cx = tx(t), cy = ty(t);
    if (can_spawn_at(t)) return t;
    for (r = 2; r < 14; r += 2)
        for (tries = 0; tries < 12; tries++) {
            int16_t x = (int16_t)(cx + rnd_n(2 * r + 1) - r);
            int16_t y = (int16_t)(cy + rnd_n(2 * r + 1) - r);
            if (x > 0 && y > 0 && x < (int16_t)g_W && y < (int16_t)g_H &&
                can_spawn_at(TILE(x, y)))
                return TILE(x, y);
        }
    return NO_TILE;
}

void game_place_ai(void)
{
    uint8_t order[MAX_MAP_NATIONS], i, n = g_mi.nnations, placed = 0;
    char nm[NATION_NAME_LEN];
    for (i = 0; i < n; i++) order[i] = i;
    for (i = n; i > 1; i--) {
        uint8_t j = (uint8_t)rnd_n(i), t = order[i - 1];
        order[i - 1] = order[j];
        order[j] = t;
    }
    for (i = 0; i < n && placed < g_cfg.nations && g_nplayers < MAXP - 1; i++) {
        const map_nation_t *mn = &g_nat[order[i]];
        tile_t t = free_spot_near(TILE(mn->x, mn->y));
        uint8_t p;
        if (t == NO_TILE) continue;
        p = add_player(PT_NATION, mn->name);
        game_spawn(p, t);
        placed++;
    }
    placed = 0;
    for (i = 0; i < 250 && placed < g_cfg.tribes && g_nplayers < MAXP - 1; i++) {
        tile_t t = random_land();
        uint8_t p, k;
        if (t == NO_TILE || !can_spawn_at(t)) continue;
        memcpy(nm, "Tribe ", 6);
        k = (uint8_t)(placed + 1);
        nm[6] = (char)('0' + k / 10);
        nm[7] = (char)('0' + k % 10);
        nm[8] = 0;
        p = add_player(PT_TRIBE, nm);
        game_spawn(p, t);
        placed++;
    }
}

void kill_player(uint8_t p, uint8_t killer)
{
    player_t *pl = &g_pl[p];
    uint8_t i;
    if (!pl->alive) return;
    pl->alive = 0;
    if (killer && g_pl[killer].alive) g_pl[killer].gold += pl->gold;
    pl->gold = 0;
    pl->troops = 0;
    pl->growth = 0;
    pl->maxTroops = 0;
    attacks_drop_player(p);
    units_drop_player(p);
    for (i = 1; i <= g_nplayers; i++) {
        MASKCLR(g_pl[i].allies, p);
        MASKCLR(g_pl[i].nbrs, p);
        MASKCLR(g_pl[i].reqFrom, p);
        if (g_pl[i].target == p) g_pl[i].target = 0;
        if (g_pl[i].aiEnemy == p) g_pl[i].aiEnemy = 0;
    }
    pl->allies = 0;
    ui_event(EV_DEAD, p, killer);
}

/* troop growth and gold of one player for one tick (src/asm/econ.s on the
 * calculator, which comes here for the rare cases) */
__attribute__((used)) void econ_grow_c(player_t *pl)
{
    uint32_t T = pl->troops, M = pl->maxTroops;
    pl->growth = 0;
    if (T >= M) {
        if (T > M) pl->troops = T - ((T - M) >> 5) - 1;
    } else {
        /* (1 - troops / max) in Q8, OpenFront's: (max - tr) / (max / 256 + 1),
         * always < 256 */
        uint8_t r8 = (uint8_t)((M - T) / ((M >> 8) + 1));
        /* with a troop multiplier everything scales by m: grow as the
         * unscaled game would, then scale the result */
        uint16_t m = g_mulQ8[g_cfg.troopMul];
        /* x^0.73 < 2^24 for any 32-bit x */
        uint32_t a, add = 10 + ((uint24_t)fx_pow(m == 256 ? T : (T / m) << 8, 187) >> 2);
        add = mulq8(add, r8);
        if (pl->type == PT_TRIBE)
            add >>= 1;
        else if (pl->type == PT_NATION) {
            /* (add * growMul) >> 8, growMul = 256 + lo from hard on */
            uint8_t d = g_cfg.difficulty;
            uint32_t g = mulq8(add, growLo[d]);
            add = d >= DIFF_HARD ? add + g : g;
        }
        /* add one, at most up to max */
        a = troop_mul(add) + 1;
        if (a > M - T) a = M - T;
        pl->troops = T + a;
        pl->growth = a;
    }
    pl->gold += gold_mul(pl->type == PT_TRIBE ? 50 : 100);
}

/* every 16 ticks: new troop cap, traitor status and grudges fade (1 point
 * per 16 ticks toward neutral, OpenFront 0.05/tick) */
static void econ_slow(player_t *pl)
{
    PROF(2, calc_max(pl));
    if (pl->traitor) pl->traitor = pl->traitor > 16 ? pl->traitor - 16 : 0;
    econ_fade(pl->rel + 1);
}

void game_tick(void)
{
    player_t *pl = &g_pl[1];
    uint8_t p;
    PERF_START();
    g_tick++;
    for (p = 1; p <= g_nplayers; p++, pl++)
        if (pl->alive) {
            if ((((uint8_t)g_tick + p) & 15) == 0) econ_slow(pl);
            econ_grow(pl);
        }
    PERF_ADD(0);
    attack_tick();
    PERF_ADD(1);
    units_tick();
    PERF_ADD(2);
    rail_tick();
    PERF_ADD(11);
    ai_tick();
    PERF_ADD(3);
    if ((g_tick & 15) == 7) diplo_tick();
    if ((g_tick & 31) == 0 && !g_winner) {
        for (p = 1; p <= g_nplayers; p++)
            if (g_pl[p].alive && (uint24_t)g_pl[p].tiles * 5 >= (uint24_t)g_landLeft * 4)
                g_winner = p;
    }
    PERF_ADD(11);
}

#ifdef CF_SELFTEST
/* Self-test build: the assembly of src/asm/econ.s against the C versions,
 * on random and edge inputs, and on copies of every player with random
 * troops, caps, sizes, cities, types, difficulties and multipliers. The game
 * is left as it was (generator state included). Returns the mismatches. */
uint16_t econ_selftest(void)
{
    /* scratch: the route search heap (free outside a search) */
    player_t *a = (player_t *)g_mem->hq, *b = a + 1, *s = a + 2;
    game_cfg_t cfg = g_cfg;
    uint8_t st[4], S = g_S, p;
    uint16_t bad = 0, k;
    rnd_get(st);
    for (k = 0; k < 4000; k++) {
        uint24_t d = ((uint24_t)rnd() << (k % 9)) ^ rnd(), n = d >> (rnd() & 15), x = rnd();
        uint8_t g = (uint8_t)rnd();
        if (k & 1) n = d;
        if (k < 16) d = n = 0xFFFFFF - k;
        if (k < 256) g = (uint8_t)k, x = 0xFFFF - (k & 3);
        bad += econ_div8(n, d) != econ_div8_c(n, d);
        bad += econ_mulq8(x, g) != econ_mulq8_c(x, g);
    }
    for (k = 0; k < 64; k++) {
        int8_t *r1 = (int8_t *)a, *r2 = (int8_t *)b;
        for (p = 0; p < MAXP; p++) r1[p] = r2[p] = (int8_t)(k < 8 ? 128 - k * 32 + p : rnd());
        econ_fade(r1 + 1);
        econ_fade_c(r2 + 1);
        bad += memcmp(r1, r2, MAXP) != 0;
    }
    for (k = 0; k < 120; k++) {
        g_cfg.difficulty = (uint8_t)(k & 3);
        g_cfg.goldMul = k < 40 ? 1 : (uint8_t)(k % 6);
        g_cfg.troopMul = k < 40 ? 1 : (uint8_t)(k / 6 % 6);
        g_S = (uint8_t)(12 + rnd_n(29));
        for (p = 1; p <= g_nplayers; p++) {
            uint32_t r = (uint32_t)rnd() << 16 | rnd();
            memcpy(s, &g_pl[p], sizeof *s);
            if (k) {
                s->type = (uint8_t)(1 + rnd_n(3));
                s->maxTroops = r >> (k % 13);
                s->troops = (uint32_t)rnd() << (k % 17);
                switch (k % 5) {
                case 0: s->troops = s->maxTroops; break;
                case 1: s->troops = s->maxTroops - rnd_n(3); break;
                case 2: s->troops = rnd_n(3); break;
                }
                s->tiles = (uint16_t)(rnd() >> (k % 16));
                s->cityLv = k & 4 ? 0 : (uint16_t)(rnd() >> (k % 16));
                s->gold = r ^ 0x5A5A;
            }
            memcpy(a, s, sizeof *s);
            memcpy(b, s, sizeof *s);
            econ_grow(a);
            econ_grow_c(b);
            bad += memcmp(a, b, sizeof *a) != 0;
            memcpy(a, s, sizeof *s);
            memcpy(b, s, sizeof *s);
            calc_max(a);
            calc_max_c(b);
            bad += memcmp(a, b, sizeof *a) != 0;
        }
    }
    g_cfg = cfg;
    g_S = S;
    rnd_set(st);
    return bad;
}
#endif
