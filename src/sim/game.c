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
static const uint16_t growMul[4] = {230, 243, 256, 269};

uint32_t gold_mul(uint32_t g)
{
    uint16_t m = g_mulQ8[g_cfg.goldMul];
    return m == 256 ? g : (g >> 8) * m + (((g & 255) * m) >> 8);
}

static uint32_t troop_mul(uint32_t t)
{
    uint16_t m = g_mulQ8[g_cfg.troopMul];
    return m == 256 ? t : (t >> 8) * m + (((t & 255) * m) >> 8);
}

bool can_trade(uint8_t a, uint8_t b)
{
    return !MASKBIT(g_pl[a].embargo, b) && !MASKBIT(g_pl[b].embargo, a);
}

#define TROOP_CAP 60000000UL

/* (num * 256) / den for num <= den, as OpenFront's (1 - troops / max):
 * num / (den / 256 + 1), with a 24-bit division whenever the values fit */
static uint16_t ratio8(uint32_t num, uint32_t den)
{
    union {
        uint32_t v;
        uint8_t b[4];
    } d, q;
    uint32_t r;
    d.v = den;
    q.b[0] = d.b[1];
    q.b[1] = d.b[2];
    q.b[2] = d.b[3];
    q.b[3] = 0; /* q = den >> 8, without a shift loop */
    q.v++;
    if (!d.b[3])
        r = (uint24_t)num / (uint24_t)q.v;
    else
        r = num / q.v;
    return (uint16_t)(r > 256 ? 256 : r);
}

static void calc_max(uint8_t p)
{
    player_t *pl = &g_pl[p];
    uint32_t n = (uint32_t)pl->tiles * g_S, m;
    int16_t l;
    /* 2 * (n^0.6 * 1000 + 50000) + cities */
    m = fx_exp2((int16_t)((((uint32_t)fx_log2(n) * 154) >> 8) + 2048)); /* Q8 */
    m = 2 * (((m * 125) >> 5) + 50000) + (uint32_t)pl->cityLv * 250000;
    if (pl->type == PT_TRIBE)
        m /= 3;
    else if (pl->type == PT_NATION)
        m = (m >> 2) * (2 + g_cfg.difficulty);
    m = troop_mul(m);
    if (m > TROOP_CAP) m = TROOP_CAP;
    pl->maxTroops = m;

    /* large territory sigmoid: 1 / (1 + (300000/n)^2.5) */
    l = (int16_t)(fx_log2(n ? n : 1) - 4658);
    l = (int16_t)(l * 5 / 2);
    if (l < -2000)
        pl->big8 = 0;
    else if (l > 2000)
        pl->big8 = 255;
    else {
        uint32_t x8 = fx_exp2((int16_t)(l + 2048));
        pl->big8 = (uint8_t)((x8 * 255) / (256 + x8));
    }
}

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
    calc_max(p);
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

static void economy(uint8_t p)
{
    player_t *pl = &g_pl[p];
    uint32_t add, max = pl->maxTroops, tr = pl->troops;

    if (((g_tick + p) & 15) == 0) {
        PROF(2, calc_max(p));
        max = pl->maxTroops;
        if (pl->traitor) pl->traitor = pl->traitor > 16 ? pl->traitor - 16 : 0;
    }
    pl->growth = 0;
    if (tr >= max) {
        if (tr > max) pl->troops = tr - ((tr - max) >> 5) - 1;
    } else {
        uint16_t r8 = ratio8(max - tr, max);
        /* with a troop multiplier everything scales by m: grow as the
         * unscaled game would, then scale the result */
        uint16_t m = g_mulQ8[g_cfg.troopMul];
        uint32_t trn = m == 256 ? tr : (tr / m) << 8;
        /* x^0.73 < 2^24 for any 32-bit x: shift it as a 24-bit value */
        add = 10 + ((uint24_t)fx_pow(trn, 187) >> 2);
        if (add < 65536) {
            union {
                uint24_t v;
                uint8_t b[3];
            } pr;
            u16b_t lo;
            pr.v = (uint24_t)add * r8;
            lo.b[0] = pr.b[1];
            lo.b[1] = pr.b[2];
            add = lo.v; /* (add * r8) >> 8 */
        } else
            add = (add * r8) >> 8;
        if (pl->type == PT_TRIBE)
            add >>= 1;
        else if (pl->type == PT_NATION) {
            uint16_t gm = growMul[g_cfg.difficulty];
            add = add < 62000 ? ((uint24_t)add * gm) >> 8 : (add * gm) >> 8; /* 24 bits when it fits */
        }
        if (m != 256) add = troop_mul(add);
        add++;
        if (tr + add > max) add = max - tr;
        pl->troops = tr + add;
        pl->growth = add;
    }
    pl->gold += g_cfg.goldMul == 1 ? (pl->type == PT_TRIBE ? 50 : 100)
                                   : gold_mul(pl->type == PT_TRIBE ? 50 : 100);
}

void game_tick(void)
{
    uint8_t p;
    PERF_START();
    g_tick++;
    for (p = 1; p <= g_nplayers; p++)
        if (g_pl[p].alive) economy(p);
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
            if (g_pl[p].alive && (uint32_t)g_pl[p].tiles * 5 >= (uint32_t)g_landLeft * 4)
                g_winner = p;
    }
    PERF_ADD(11);
}
