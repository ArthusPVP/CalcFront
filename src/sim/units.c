/* structures, nukes and shared unit helpers */
#include "sim.h"

unit_t g_un[MAX_UNITS];

/* construction time in ticks: city post port factory silo sam */
static const uint16_t buildTime[7] = {0, 20, 50, 50, 20, 100, 300};

uint8_t orig2tiles(uint16_t d)
{
    uint16_t r = (uint16_t)((d * 16u + g_L16 / 2) / g_L16);
    return r < 1 ? 1 : (r > 250 ? 250 : (uint8_t)r);
}

uint8_t sam_range(uint8_t level) { return orig2tiles((uint16_t)(150 - 480 / (level + 5))); }

void nuke_radius(uint8_t type, uint8_t *inner, uint8_t *outer)
{
    if (type == U_HYDRO) {
        *inner = orig2tiles(80);
        *outer = orig2tiles(100);
    } else if (type == U_MIRV) {
        /* each warhead covers 10 times OpenFront's area (radius x 3.16) */
        *inner = orig2tiles(38);
        *outer = orig2tiles(57);
    } else {
        *inner = orig2tiles(12);
        *outer = orig2tiles(30);
    }
}

uint8_t g_unHi; /* every unit lives below this index */

/* recount() is skipped while nothing it counts has changed: set by every
 * change of a counted unit (type below NCOUNT: creation, deletion, owner,
 * level, a post being finished) and by build() */
uint8_t g_unChanged = 1;
static uint24_t rcTick;
#ifdef CF_CHECKRC
unsigned long g_rcSkip;
#endif
#define NCOUNT (U_WARSHIP + 1)

unit_t *new_unit(uint8_t type, uint8_t owner, tile_t pos)
{
    uint8_t i;
    for (i = 0; i < MAX_UNITS; i++)
        if (g_un[i].type == U_NONE) {
            if (i >= g_unHi) g_unHi = (uint8_t)(i + 1);
            unit_t *u = &g_un[i];
            memset(u, 0, sizeof *u);
            u->type = type;
            u->owner = owner;
            u->pos = pos;
            u->path = 0xFF;
            u->level = 1;
            if (type < NCOUNT) g_unChanged = 1;
            return u;
        }
    return 0;
}

void del_unit(unit_t *u)
{
    if (IS_STRUCT(u->type)) {
        st_set(u->pos, false);
        if (u->type == U_CITY && BUILT(u)) g_pl[u->owner].cityLv -= u->level;
        if (IS_STATIONABLE(u->type)) rail_mark_dirty();
        ui_dirty_unit(u->pos);
    }
    if (u->type == U_BOAT && g_pl[u->owner].boats) g_pl[u->owner].boats--;
    if (u->type < NCOUNT) g_unChanged = 1;
    path_release(u->path);
    u->path = 0xFF;
    u->type = U_NONE;
}

/* coordinates and slots of every building (refreshed by recount(), appended
 * to by build()), for the spacing rule and fast lookups: un_st[0] and
 * un_st[1] hold the low and high bytes of their tiles, un_st[2] their slots */
static uint16_t stX[MAX_UNITS], stY[MAX_UNITS];
uint8_t un_st[3][MAX_UNITS];
uint8_t un_nSt;
#define stIdx (un_st[2])
#define nSt un_nSt

static void st_add(uint8_t i, tile_t t)
{
    u16b_t v;
    uint16_t y = ty(t);
    v.v = t;
    un_st[0][nSt] = v.b[0];
    un_st[1][nSt] = v.b[1];
    stIdx[nSt] = i;
    stY[nSt] = y;
    stX[nSt++] = (uint16_t)(t - g_rowStart[y]);
}

/* trade ships and trains seen by units_next() */
uint8_t ut_cnt[2];

#if defined(HOST) || defined(CF_NOASM) || defined(CF_SELFTEST)
/* The C versions of src/asm/units.s (see there) */
uint8_t units_next_c(uint8_t i)
{
    uint8_t hi = g_unHi;
    for (; i < hi; i++) {
        unit_t *u = &g_un[i];
        uint8_t t = u->type;
        if (t == U_NONE) continue;
        if (t == U_TRADE) ut_cnt[0]++;
        if (t == U_TRAIN) ut_cnt[1]++;
        if (t <= U_SAM) {
            if (!u->timer) {
                if ((t == U_PORT || t == U_FACTORY) && !(((uint8_t)g_tick + i) & 7)) return i;
                continue;
            }
        } else if (t == U_WARSHIP || (t >= U_ATOM && t <= U_SHELL))
            return i;
        else if (t >= U_TYPES)
            continue;
        if (u->timer == 1) return i; /* runs out: C's turn */
        u->timer--;
    }
    return i;
}

int16_t st_find_c(tile_t t)
{
    u16b_t v;
    uint8_t j;
    v.v = t;
    for (j = 0; j < nSt; j++) {
        unit_t *u;
        if (un_st[0][j] != v.b[0] || un_st[1][j] != v.b[1]) continue;
        u = &g_un[stIdx[j]];
        if (u->pos == t && IS_STRUCT(u->type)) return stIdx[j];
    }
    return -1;
}
#endif
#if defined(HOST) || defined(CF_NOASM)
#define units_next units_next_c
#define st_find st_find_c
#else
uint8_t units_next(uint8_t i);
int16_t st_find(tile_t t);
_Static_assert(sizeof(unit_t) == 32 && offsetof(unit_t, pos) == 2 && offsetof(unit_t, timer) == 10,
               "unit_t offsets used by src/asm/units.s");
_Static_assert(U_PORT == 3 && U_FACTORY == 4 && U_SAM == 6 && U_TRADE == 8 && U_WARSHIP == 9 &&
                   U_SHELL == 13 && U_TRAIN == 14 && U_BOOM == 15 && U_TYPES == 16 && MAX_UNITS == 200,
               "constants used by src/asm/units.s");
#endif

/* There is at most one structure per tile (build() upgrades the one standing
 * there, and only deleting it clears the tile's structure bit), so finding it
 * through the list (where an entry left by a deleted one is skipped) gives
 * the same slot as any other search. */
#ifdef CF_CHECKRC
static int16_t unit_at_orig(tile_t t)
{
    uint8_t i;
    if (!has_struct(t)) return -1;
    for (i = 0; i < nSt; i++) {
        unit_t *u = &g_un[stIdx[i]];
        if (u->pos == t && IS_STRUCT(u->type)) return stIdx[i];
    }
    for (i = 0; i < g_unHi; i++)
        if (g_un[i].pos == t && IS_STRUCT(g_un[i].type)) return i;
    return -1;
}
static int16_t unit_at2(tile_t t);
int16_t unit_at(tile_t t)
{
    int16_t a = unit_at2(t), b = unit_at_orig(t);
    uint8_t i, n = 0;
    extern int printf(const char *, ...);
    if (a != b) printf("!! unit_at %d %d\n", a, b);
    for (i = 0; i < g_unHi; i++)
        if (g_un[i].pos == t && IS_STRUCT(g_un[i].type)) n++;
    if (n > 1) printf("!! two structures on %u\n", t);
    return a;
}
#define unit_at unit_at2
static
#endif
int16_t unit_at(tile_t t)
{
    uint8_t i;
    int16_t k;
    if (!has_struct(t)) return -1;
    if ((k = st_find(t)) >= 0) return k;
    /* not listed: the list was full */
    for (i = 0; i < g_unHi; i++)
        if (g_un[i].pos == t && IS_STRUCT(g_un[i].type)) return i;
    return -1;
}
#undef unit_at

/* per-player unit counts and structure levels, recounted at the start of a
 * tick (when something changed, see g_unChanged) and
 * bumped by build() so decisions within a tick see their own builds */
static uint8_t cntTab[NCOUNT][MAXP]; /* [type][player]: no multiply */
static uint16_t lvTab[NCOUNT][MAXP];
#define MAX_POSTS 48
static tile_t postPos[MAX_POSTS];
static uint8_t postOwn[MAX_POSTS], nPosts;


uint8_t count_units(uint8_t p, uint8_t type) { return type < NCOUNT ? cntTab[type][p] : 0; }

uint16_t levels_of(uint8_t p, uint8_t type) { return type < NCOUNT ? lvTab[type][p] : 0; }

static void recount(void)
{
    uint8_t i, hi = g_unHi;
    unit_t *u = g_un;
    g_unChanged = 0;
    rcTick = g_tick;
    memset(cntTab, 0, sizeof cntTab);
    memset(lvTab, 0, sizeof lvTab);
    nPosts = 0;
    nSt = 0;
    for (i = 0; i < hi; i++, u++) {
        if (u->type == U_NONE || u->type >= NCOUNT) continue;
        if (IS_STRUCT(u->type)) st_add(i, u->pos);
        cntTab[u->type][u->owner]++;
        lvTab[u->type][u->owner] += u->level;
        if (u->type == U_POST && BUILT(u) && nPosts < MAX_POSTS) {
            postPos[nPosts] = u->pos;
            postOwn[nPosts++] = u->owner;
        }
    }
}

static uint32_t doubling(uint16_t n) { return n >= 3 ? 1000000 : (125000UL << n); }

/* OpenFront prices: they rise with what you already own (levels count) */
uint32_t unit_cost(uint8_t p, uint8_t type)
{
    uint32_t c;
    switch (type) {
    case U_CITY:
        return doubling(levels_of(p, U_CITY));
    case U_PORT:
    case U_FACTORY:
        return doubling(levels_of(p, U_PORT) + levels_of(p, U_FACTORY));
    case U_POST:
        c = (count_units(p, U_POST) + 1) * 50000UL;
        return c > 250000 ? 250000 : c;
    case U_SILO:
        return 1000000;
    case U_SAM:
        return levels_of(p, U_SAM) ? 3000000 : 1500000;
    case U_WARSHIP:
        c = (count_units(p, U_WARSHIP) + 1) * 250000UL;
        return c > 1000000 ? 1000000 : c;
    case U_ATOM:
        return 750000;
    case U_HYDRO:
        return 5000000;
    case U_MIRV:
        return 25000000UL + g_mirvs * 15000000UL;
    }
    return 0;
}

bool post_near(uint8_t owner, tile_t t)
{
    uint8_t i, r = orig2tiles(30);
    for (i = 0; i < nPosts; i++)
        if (postOwn[i] == owner && near(postPos[i], t, r)) return true;
    return false;
}

bool can_build(uint8_t p, uint8_t type, tile_t t)
{
    uint8_t b = g_map[t], i, r;
    if (type == U_WARSHIP) {
        if (IS_LAND(b) || !(b & T_COMP)) return false;
        return port_on(p, b & T_COMP, t) != 0;
    }
    if (!IS_STRUCT(type) || !IS_LAND(b) || (b & T_OWNER) != p) return false;
    if (has_struct(t)) {
        int16_t k = unit_at(t);
        return k >= 0 && g_un[k].type == type && type != U_POST && BUILT(&g_un[k]) &&
               g_un[k].level < MAX_LEVEL;
    }
    if (type == U_PORT && !is_shore(t)) return false;
    r = orig2tiles(15);
    if (r < 3) r = 3;
    {
        uint16_t y = ty(t), x = (uint16_t)(t - g_rowStart[y]);
        uint16_t r2 = (uint16_t)r * r;
        for (i = 0; i < nSt; i++) {
            int16_t dx = (int16_t)(stX[i] - x), dy = (int16_t)(stY[i] - y);
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx <= r && dy <= r && (uint16_t)(dx * dx + dy * dy) <= r2) return false;
        }
    }
    return true;
}

bool build(uint8_t p, uint8_t type, tile_t t)
{
    player_t *pl = &g_pl[p];
    uint32_t cost = unit_cost(p, type);
    unit_t *u;
    bool up;
    if (!can_build(p, type, t)) {
        if (p == HUMAN) ui_event(EV_BADSPOT, 0, 0);
        return false;
    }
    if (pl->gold < cost) {
        if (p == HUMAN) ui_event(EV_NOGOLD, 0, 0);
        return false;
    }
    up = type != U_WARSHIP && has_struct(t);
    if (type == U_WARSHIP) {
        if (!warship_spawn(p, t)) return false;
    } else if (up) {
        u = &g_un[unit_at(t)];
        u->level++;
        if (type == U_CITY) pl->cityLv++;
        ui_dirty_unit(t);
    } else {
        u = new_unit(type, p, t);
        if (!u) return false;
        u->timer = buildTime[type];
        st_set(t, true);
        ui_dirty_unit(t);
        if (nSt < MAX_UNITS) st_add((uint8_t)(u - g_un), t);
    }
    pl->gold -= cost;
    g_unChanged = 1;
    cntTab[type][p] += !up;
    lvTab[type][p]++;
    return true;
}

void unit_on_conquer(tile_t t, uint8_t newOwner)
{
    int16_t i = unit_at(t);
    unit_t *u;
    if (i < 0) {
        st_set(t, false);
        return;
    }
    u = &g_un[i];
    if (u->owner == newOwner) return;
    if (u->owner == HUMAN) ui_event(EV_LOST_UNIT, u->type, newOwner);
    if (u->type == U_POST || !BUILT(u)) {
        del_unit(u);
        return;
    }
    if (u->type == U_CITY) {
        g_pl[u->owner].cityLv -= u->level;
        g_pl[newOwner].cityLv += u->level;
    }
    u->owner = newOwner;
    g_unChanged = 1;
    ui_dirty_unit(t);
}

void units_drop_player(uint8_t p)
{
    uint8_t i;
    for (i = 0; i < g_unHi; i++) {
        unit_t *u = &g_un[i];
        if (u->type != U_NONE && u->owner == p) del_unit(u);
    }
}

/* ---------------------------------------------------------------- nukes --- */
int16_t find_silo(uint8_t p)
{
    uint8_t i;
    for (i = 0; i < g_unHi; i++)
        if (g_un[i].type == U_SILO && g_un[i].owner == p && BUILT(&g_un[i]) && !g_un[i].timer)
            return i;
    return -1;
}

bool launch_nuke(uint8_t p, uint8_t type, tile_t target)
{
    player_t *pl = &g_pl[p];
    uint32_t cost = unit_cost(p, type);
    uint8_t i, victim;
    unit_t *silo = 0, *u;
    uint16_t bd = 0xFFFF, d;
    for (i = 0; i < g_unHi; i++) {
        unit_t *s = &g_un[i];
        if (s->type == U_SILO && s->owner == p && BUILT(s) && !s->timer) {
            d = tdist(s->pos, target);
            if (d < bd) {
                bd = d;
                silo = s;
            }
        }
    }
    if (!silo) {
        if (p == HUMAN) ui_event(EV_NOSILO, 0, 0);
        return false;
    }
    if (pl->gold < cost) {
        if (p == HUMAN) ui_event(EV_NOGOLD, 0, 0);
        return false;
    }
    u = new_unit(type, p, silo->pos);
    if (!u) return false;
    pl->gold -= cost;
    u->src = silo->pos;
    u->dst = target;
    u->timer = 0;
    d = (uint16_t)(((uint32_t)bd * g_L16) / (type == U_MIRV ? 240 : 160)) + 2;
    u->timer2 = d;
    silo->timer = 75;
    if (type == U_MIRV) g_mirvs++;
    victim = IS_LAND(g_map[target]) ? OWNER_OF(target) : 0;
    u->aux = victim;
    if (victim && victim != p) {
        rel_add(victim, p, -100);
        if (victim == HUMAN) ui_event(EV_NUKE_IN, p, type);
        if (ALLIED(p, victim)) ally_break(p, victim);
    }
    return true;
}

static void blast(tile_t c, uint8_t rin, uint8_t rout, uint8_t by)
{
    int16_t x, y, cx = (int16_t)tx(c), cy = (int16_t)ty(c);
    uint16_t in2 = (uint16_t)rin * rin, out2 = (uint16_t)rout * rout;
    uint8_t i;
    unit_t *u;
    for (y = cy - rout; y <= cy + rout; y++) {
        if (y < 1 || y >= (int16_t)g_H - 1) continue;
        for (x = cx - rout; x <= cx + rout; x++) {
            uint16_t d2;
            tile_t t;
            uint8_t b, o;
            if (x < 1 || x >= (int16_t)g_W - 1) continue;
            d2 = (uint16_t)((x - cx) * (x - cx) + (y - cy) * (y - cy));
            if (d2 > out2 || (d2 > in2 && (rnd() & 1))) continue;
            t = TILE(x, y);
            b = g_map[t];
            if (!IS_LAND(b)) continue;
            o = b & T_OWNER;
            if (o) {
                player_t *v = &g_pl[o];
                sub_troops(o, v->troops / (v->tiles + 1u));
                if (o != by) rel_add(o, by, -5);
            }
            relinquish(t);
        }
    }
    for (i = 0; i < g_unHi; i++) {
        u = &g_un[i];
        if (u->type == U_NONE || IS_NUKE(u->type) || u->type == U_BOOM || u->type == U_SHELL)
            continue;
        if (near(u->pos, c, rout)) {
            if (u->owner == HUMAN && u->type != U_TRAIN) ui_event(EV_LOST_UNIT, u->type, by);
            del_unit(u);
        }
    }
    u = new_unit(U_BOOM, by, c);
    if (u) {
        u->level = rout;
        u->timer = 8;
    }
}

/* is there a ready SAM protecting this spot against nukes of `by`? */
static unit_t *sam_cover(tile_t pos, uint8_t by, uint8_t victim)
{
    uint8_t i;
    for (i = 0; i < g_unHi; i++) {
        unit_t *s = &g_un[i];
        if (s->type != U_SAM || !BUILT(s) || s->timer || s->owner == by) continue;
        if (s->owner != victim && !(victim && ALLIED(s->owner, victim))) continue;
        if (near(s->pos, pos, sam_range(s->level))) return s;
    }
    return 0;
}

/* A MIRV that has landed keeps falling as warheads over the victim's land,
 * one per tick (level = warheads left, dist = random tries left): 28 big
 * blasts at once would stall the game for seconds. */
static void mirv_warhead(unit_t *u)
{
    uint8_t rin, rout, victim = u->aux;
    player_t *v = &g_pl[victim];
    nuke_radius(U_MIRV, &rin, &rout);
    while (u->level && u->dist) {
        tile_t t;
        unit_t *s;
        u->dist--;
        if (!victim || !v->alive) break;
        t = rnd_tile(v->minx, (uint16_t)(v->maxx - v->minx + 1), v->miny,
                     (uint16_t)(v->maxy - v->miny + 1));
        if (!IS_LAND(g_map[t]) || OWNER_OF(t) != victim) continue;
        u->level--;
        u->pos = t;
        s = sam_cover(t, u->owner, victim);
        if (s && (rnd() & 1)) {
            s->timer = 20;
            return;
        }
        blast(t, rin, rout, u->owner);
        return;
    }
    u->type = U_NONE; /* no warheads or no target left */
}

static void nuke_tick(unit_t *u)
{
    uint8_t rin, rout;
    if (u->timer >= u->timer2) {
        mirv_warhead(u);
        return;
    }
    u->timer++;
    if (u->timer >= u->timer2) {
        tile_t c = u->dst;
        uint8_t by = u->owner, type = u->type;
        nuke_radius(type, &rin, &rout);
        if (type == U_MIRV) {
            u->pos = c;
            u->level = 28;
            u->dist = 200;
        } else
            u->type = U_NONE;
        blast(c, rin, rout, by);
        return;
    }
    u->pos = line_point(u->src, u->dst, u->timer, u->timer2);
    if (u->type != U_MIRV && (u->timer & 1)) {
        unit_t *s = sam_cover(u->pos, u->owner, u->aux);
        if (s) {
            s->timer = 75;
            if (u->owner == HUMAN || s->owner == HUMAN) ui_event(EV_SAM_HIT, s->owner, 0);
            u->type = U_NONE;
        }
    }
}

/* counts and caches normally refreshed by units_tick (after loading a game) */
void units_refresh(void)
{
    uint8_t i;
    recount();
    g_nTrade = g_nTrain = 0;
    for (i = 0; i < g_unHi; i++) {
        if (g_un[i].type == U_TRADE) g_nTrade++;
        if (g_un[i].type == U_TRAIN) g_nTrain++;
    }
}

/* ----------------------------------------------------------------- tick --- */
void units_tick(void)
{
    uint8_t i;
    if (g_unChanged || g_tick != rcTick + 1 || g_tick == 1) PROF(3, recount());
    else {
#ifdef CF_CHECKRC
        static uint8_t c0[sizeof cntTab], l0[sizeof lvTab], s0[sizeof un_st], p1[MAX_POSTS];
        static uint16_t x0[MAX_UNITS], y0[MAX_UNITS], p0[MAX_POSTS];
        uint8_t n0 = nSt, n1 = nPosts;
        memcpy(c0, cntTab, sizeof c0); memcpy(l0, lvTab, sizeof l0); memcpy(s0, un_st, sizeof s0);
        memcpy(x0, stX, sizeof x0); memcpy(y0, stY, sizeof y0); memcpy(p0, postPos, sizeof p0); memcpy(p1, postOwn, sizeof p1);
        recount();
        if (n0 != nSt || n1 != nPosts || memcmp(c0, cntTab, sizeof c0) || memcmp(l0, lvTab, sizeof l0) ||
            memcmp(s0, un_st[0], nSt) || memcmp(s0 + MAX_UNITS, un_st[1], nSt) || memcmp(s0 + 2 * MAX_UNITS, un_st[2], nSt) || memcmp(x0, stX, 2 * nSt) || memcmp(y0, stY, 2 * nSt) ||
            memcmp(p0, postPos, 2 * nPosts) || memcmp(p1, postOwn, nPosts)) {
            extern int printf(const char *, ...);
            printf("!! recount skipped wrongly at tick %u\n", (unsigned)g_tick);
        }
        g_rcSkip++;
        if (g_tick % 2000 == 0) { extern int printf(const char *, ...); printf("rcskip %lu at %u\n", g_rcSkip, (unsigned)g_tick); }
#endif
        rcTick = g_tick;
    }
    for (i = 1; i <= g_nplayers; i++) g_pl[i].nPost = 0;
    for (i = 0; i < nPosts; i++) g_pl[postOwn[i]].nPost++;
    /* the units that need more than a timer counted down */
    ut_cnt[0] = ut_cnt[1] = 0;
    for (i = units_next(0); i < g_unHi; i = units_next((uint8_t)(i + 1))) {
        unit_t *u = &g_un[i];
        switch (u->type) {
        case U_CITY:
        case U_POST:
        case U_PORT:
        case U_FACTORY:
        case U_SILO:
        case U_SAM:
            if (u->timer) {
                if (--u->timer == 0 && !BUILT(u)) {
                    u->hp = 1;
                    g_unChanged = 1;
                    if (u->type == U_CITY) g_pl[u->owner].cityLv += u->level;
                    if (IS_STATIONABLE(u->type)) rail_mark_dirty();
                    ui_dirty_unit(u->pos);
                }
            } else if (u->type == U_PORT)
                PROF(5, port_tick(i));
            else if (u->type == U_FACTORY)
                PROF(7, factory_tick(i));
            break;
        case U_BOAT:
            PROF(6, boat_tick(u));
            break;
        case U_TRADE:
            PROF(6, trade_tick(u));
            break;
        case U_WARSHIP:
            PROF(4, warship_tick(i));
            break;
        case U_SHELL:
            shell_tick(u);
            break;
        case U_TRAIN:
            PROF(7, train_tick(u));
            break;
        case U_ATOM:
        case U_HYDRO:
        case U_MIRV:
            nuke_tick(u);
            break;
        case U_BOOM:
            if (--u->timer == 0) u->type = U_NONE;
            break;
        }
    }
    g_nTrade = ut_cnt[0];
    g_nTrain = ut_cnt[1];
    while (g_unHi && g_un[g_unHi - 1].type == U_NONE) g_unHi--;
}

#ifdef CF_SELFTEST
/* ------------------------------------------------------------ self-test --- */
/* random changes to the units and the structure list (types, timers, list
 * entries made stale, a few listed structures, tick, end of the units; none
 * in round 0), then a start slot and tiles to look up, mostly listed ones */
static void st_mess(uint16_t round, uint8_t *from, tile_t *t)
{
    static const uint16_t tm[8] = {0, 1, 2, 255, 256, 257, 0xFFFF, 3};
    uint8_t k, n = round ? (uint8_t)rnd_n(12) : 0;
    for (k = 0; k < n; k++) {
        unit_t *u = &g_un[rnd_n(MAX_UNITS)];
        uint8_t r = (uint8_t)rnd_n(3), j = (uint8_t)rnd_n(MAX_UNITS);
        if (rnd() & 1) u->type = (uint8_t)rnd_n(U_TYPES + 2);
        u->timer = (rnd() & 1) ? tm[rnd() & 7] : rnd();
        if (!(rnd() & 3)) un_st[r][j] = (uint8_t)(r == 2 ? rnd_n(MAX_UNITS) : rnd());
    }
    if (round) {
        for (k = 0; k < 3; k++) { /* idle listed structures, whatever the game has */
            uint8_t i = (uint8_t)rnd_n(g_unHi < MAX_UNITS ? g_unHi + 1u : MAX_UNITS);
            uint8_t j = (uint8_t)rnd_n(un_nSt < MAX_UNITS ? un_nSt + 1u : MAX_UNITS);
            u16b_t v;
            g_un[i].type = (uint8_t)(U_CITY + rnd_n(U_SAM - U_CITY + 1));
            g_un[i].timer = 0;
            v.v = g_un[i].pos;
            un_st[0][j] = v.b[0];
            un_st[1][j] = v.b[1];
            un_st[2][j] = i;
            if (j == un_nSt) un_nSt++;
        }
        g_tick = rnd();
        if (!(rnd() & 3)) g_unHi = (uint8_t)rnd_n(MAX_UNITS + 1);
        if (!(rnd() & 7)) un_nSt = (uint8_t)rnd_n(MAX_UNITS + 1);
    }
    *from = (uint8_t)((rnd() & 7) ? rnd_n(g_unHi + 1u) : rnd_n(256));
    ut_cnt[0] = (uint8_t)rnd();
    ut_cnt[1] = (uint8_t)rnd();
    for (k = 0; k < 8; k++) {
        u16b_t v;
        uint8_t j = (uint8_t)rnd_n(un_nSt ? un_nSt : 1);
        v.b[0] = un_st[0][j];
        v.b[1] = un_st[1][j];
        switch (rnd() & 7) {
        case 0: v.v = rnd(); break;
        case 1: v.v = g_un[rnd_n(MAX_UNITS)].pos; break;
        case 2: v.v++; break;
        case 3: v.v += 256; break;
        }
        t[k] = v.v;
    }
}

static uint24_t timer_sum(void)
{
    uint24_t s = 0;
    uint8_t j;
    for (j = 0; j < MAX_UNITS; j++) s += g_un[j].timer;
    return s;
}

/* the assembly (src/asm/units.s) against its C versions, on copies of the
 * live units with random changes; returns the number of mismatches (plus 1
 * if some kind of case never came up) and leaves everything (the generator
 * too) as it was */
uint16_t units_selftest(void)
{
    /* scratch: the route search buffers (free outside a search) */
    uint8_t *save = (uint8_t *)g_mem->hq, *res = save + sizeof g_un, *lst = res + 24 * MAX_UNITS;
    uint8_t st[4], s1[4], hi0 = g_unHi, n0 = un_nSt, c0 = ut_cnt[0], c1 = ut_cnt[1], seen = 0;
    uint24_t tick0 = g_tick;
    uint16_t bad = 0, k, j;
    _Static_assert(sizeof g_un + 24 * MAX_UNITS + sizeof un_st <= 3 * sizeof g_mem->hq, "scratch");
    rnd_get(st);
    memcpy(save, g_un, sizeof g_un);
    memcpy(lst, un_st, sizeof un_st);
    for (j = 0; j < un_nSt; j++) { /* every listed structure */
        u16b_t v;
        v.b[0] = un_st[0][j];
        v.b[1] = un_st[1][j];
        bad += st_find(v.v) != st_find_c(v.v);
    }
    for (k = 0; k < 600; k++) {
        uint8_t from, r1, r2, a0, a1;
        uint24_t ts;
        tile_t t[8];
        int16_t f[8];
        rnd_get(s1);
        st_mess(k, &from, t);
        r1 = units_next(from);
        a0 = ut_cnt[0];
        a1 = ut_cnt[1];
        for (j = 0; j < 8; j++) f[j] = st_find(t[j]);
        for (j = 0; j < MAX_UNITS; j++) memcpy(res + 24 * j, &g_un[j], 24);
        memcpy(g_un, save, sizeof g_un);
        memcpy(un_st, lst, sizeof un_st);
        un_nSt = n0;
        g_unHi = hi0;
        rnd_set(s1);
        st_mess(k, &from, t);
        ts = timer_sum();
        r2 = units_next_c(from);
        bad += r1 != r2 || a0 != ut_cnt[0] || a1 != ut_cnt[1];
        /* cases covered: timers counted down, stops at the end, at a timer
         * running out, at a port or factory's turn, at a warship, nuke or
         * shell; st_find hits and misses */
        if (timer_sum() < ts) seen |= 1;
        if (r2 >= g_unHi)
            seen |= 2;
        else if (g_un[r2].type == U_WARSHIP || (g_un[r2].type >= U_ATOM && g_un[r2].type <= U_SHELL))
            seen |= 4;
        else
            seen |= g_un[r2].timer == 1 ? 8 : 16;
        for (j = 0; j < 8; j++) {
            int16_t c = st_find_c(t[j]);
            bad += f[j] != c;
            seen |= c >= 0 ? 32 : 64;
        }
        for (j = 0; j < MAX_UNITS; j++) bad += memcmp(res + 24 * j, &g_un[j], 24) != 0;
        memcpy(g_un, save, sizeof g_un);
        memcpy(un_st, lst, sizeof un_st);
        un_nSt = n0;
        g_unHi = hi0;
    }
    g_tick = tick0;
    ut_cnt[0] = c0;
    ut_cnt[1] = c1;
    rnd_set(st);
    return bad + (seen != 127);
}
#endif
