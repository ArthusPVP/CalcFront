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
        *inner = orig2tiles(12);
        *outer = orig2tiles(18);
    } else {
        *inner = orig2tiles(12);
        *outer = orig2tiles(30);
    }
}

uint8_t g_unHi; /* every unit lives below this index */

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
    path_release(u->path);
    u->path = 0xFF;
    u->type = U_NONE;
}

/* coordinates and slots of every building (refreshed each tick, appended to
 * by build()), for the spacing rule and fast lookups */
static uint16_t stX[MAX_UNITS], stY[MAX_UNITS];
static uint8_t stIdx[MAX_UNITS];
static uint8_t nSt;

int16_t unit_at(tile_t t)
{
    uint8_t i;
    if (!has_struct(t)) return -1;
    for (i = 0; i < nSt; i++) {
        unit_t *u = &g_un[stIdx[i]];
        if (u->pos == t && IS_STRUCT(u->type)) return stIdx[i];
    }
    /* built this tick and not listed yet */
    for (i = 0; i < g_unHi; i++)
        if (g_un[i].pos == t && IS_STRUCT(g_un[i].type)) return i;
    return -1;
}

/* per-player unit counts and structure levels, recounted every tick and
 * bumped by build() so decisions within a tick see their own builds */
#define NCOUNT (U_WARSHIP + 1)
static uint8_t cntTab[MAXP][NCOUNT];
static uint16_t lvTab[MAXP][NCOUNT];
#define MAX_POSTS 48
static tile_t postPos[MAX_POSTS];
static uint8_t postOwn[MAX_POSTS], nPosts;


uint8_t count_units(uint8_t p, uint8_t type) { return type < NCOUNT ? cntTab[p][type] : 0; }

uint16_t levels_of(uint8_t p, uint8_t type) { return type < NCOUNT ? lvTab[p][type] : 0; }

static void recount(void)
{
    uint8_t i, hi = g_unHi;
    unit_t *u = g_un;
    memset(cntTab, 0, sizeof cntTab);
    memset(lvTab, 0, sizeof lvTab);
    nPosts = 0;
    nSt = 0;
    for (i = 0; i < hi; i++, u++) {
        if (u->type == U_NONE || u->type >= NCOUNT) continue;
        if (IS_STRUCT(u->type)) {
            uint16_t y = ty(u->pos);
            stIdx[nSt] = i;
            stY[nSt] = y;
            stX[nSt++] = (uint16_t)(u->pos - g_rowStart[y]);
        }
        cntTab[u->owner][u->type]++;
        lvTab[u->owner][u->type] += u->level;
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
        if (nSt < MAX_UNITS) {
            uint16_t y = ty(t);
            stIdx[nSt] = (uint8_t)(u - g_un);
            stY[nSt] = y;
            stX[nSt++] = (uint16_t)(t - g_rowStart[y]);
        }
    }
    pl->gold -= cost;
    cntTab[p][type] += !up;
    lvTab[p][type]++;
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

static void nuke_tick(unit_t *u)
{
    uint8_t rin, rout;
    u->timer++;
    if (u->timer >= u->timer2) {
        tile_t c = u->dst;
        uint8_t by = u->owner, type = u->type, victim = u->aux;
        u->type = U_NONE;
        nuke_radius(type, &rin, &rout);
        if (type == U_MIRV) {
            uint8_t n = 28, tries = 200;
            player_t *v = &g_pl[victim];
            blast(c, rin, rout, by);
            while (n && tries--) {
                tile_t t;
                unit_t *s;
                if (!victim || !v->alive) break;
                t = TILE(v->minx + rnd_n(v->maxx - v->minx + 1),
                         v->miny + rnd_n(v->maxy - v->miny + 1));
                if (!IS_LAND(g_map[t]) || OWNER_OF(t) != victim) continue;
                n--;
                s = sam_cover(t, by, victim);
                if (s && (rnd() & 1)) {
                    s->timer = 20;
                    continue;
                }
                blast(t, rin, rout, by);
            }
        } else
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
    uint8_t i, nt = 0, ntr = 0;
    PROF(3, recount());
    for (i = 1; i <= g_nplayers; i++) g_pl[i].nPost = 0;
    for (i = 0; i < nPosts; i++) g_pl[postOwn[i]].nPost++;
    for (i = 0; i < g_unHi; i++) {
        unit_t *u = &g_un[i];
        switch (u->type) {
        case U_NONE:
            break;
        case U_CITY:
        case U_POST:
        case U_PORT:
        case U_FACTORY:
        case U_SILO:
        case U_SAM:
            if (u->timer) {
                if (--u->timer == 0 && !BUILT(u)) {
                    u->hp = 1;
                    if (u->type == U_CITY) g_pl[u->owner].cityLv += u->level;
                    if (IS_STATIONABLE(u->type)) rail_mark_dirty();
                    if (u->owner == HUMAN) ui_event(EV_BUILT, u->type, 0);
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
            nt++;
            PROF(6, trade_tick(u));
            break;
        case U_WARSHIP:
            PROF(4, warship_tick(i));
            break;
        case U_SHELL:
            shell_tick(u);
            break;
        case U_TRAIN:
            ntr++;
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
    g_nTrade = nt;
    g_nTrain = ntr;
    while (g_unHi && g_un[g_unHi - 1].type == U_NONE) g_unHi--;
}
