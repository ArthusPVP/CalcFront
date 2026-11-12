/* ships: transport boats, ports + trade ships, warships and their shells */
#include "sim.h"

uint8_t g_nTrade, g_nTrain;

#define TRADE_CAP 30

/* ticks per tile: a ship sails about one original tile per tick */
static uint8_t ship_delay(void)
{
    uint8_t d = g_L16 >> 4;
    return d < 2 ? 2 : d;
}

unit_t *port_on(uint8_t p, uint8_t comp, tile_t nearTo)
{
    uint8_t i;
    unit_t *best = 0;
    uint16_t bd = 0xFFFF;
    for (i = 0; i < g_unHi; i++) {
        unit_t *u = &g_un[i];
        if (u->type == U_PORT && u->owner == p && BUILT(u) &&
            (!comp || water_next_to(u->pos, comp) != NO_TILE)) {
            uint16_t d = mdist(u->pos, nearTo);
            if (d < bd) {
                bd = d;
                best = u;
            }
        }
    }
    return best;
}

int16_t ship_near(tile_t t, uint8_t r, uint8_t type, uint8_t owner)
{
    uint8_t i;
    int16_t best = -1;
    uint16_t bd = 0xFFFF;
    for (i = 0; i < g_unHi; i++) {
        unit_t *u = &g_un[i];
        uint16_t d;
        if (u->type != type || (owner != 0xFF && u->owner != owner)) continue;
        d = mdist(u->pos, t);
        if (d <= r && d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

/* follow the stored route one tile: 0 moving, 1 arrived, 2 no route */
static uint8_t ship_advance(unit_t *u)
{
    int8_t d;
    if (u->pos == u->dst) return 1;
    d = path_step(u->path, u->timer2);
    if (d < 0) {
        /* end of a truncated route (or none yet): plan the rest */
        path_release(u->path);
        u->timer2 = 0;
        u->path = path_find(u->pos, u->dst);
        d = path_step(u->path, 0);
        if (d < 0) return 2;
    }
    u->timer2++;
    u->dist++;
    u->pos = (tile_t)(u->pos + g_dirofs[d]);
    return u->pos == u->dst;
}

/* one step toward a goal over open water (for short chases) */
static bool ship_greedy(unit_t *u, tile_t goal)
{
    uint8_t comp = g_map[u->pos] & T_COMP, i, best = 0xFF;
    uint16_t bd = mdist(u->pos, goal);
    for (i = 0; i < 4; i++) {
        tile_t n = (tile_t)(u->pos + g_dirofs[i]);
        uint8_t nb = g_map[n];
        uint16_t d;
        if (IS_LAND(nb) || (nb & T_COMP) != comp) continue;
        d = mdist(n, goal);
        if (d < bd || (d == bd && best == 0xFF && (rnd() & 3) == 0)) {
            bd = d;
            best = i;
        }
    }
    if (best == 0xFF) return false;
    u->pos = (tile_t)(u->pos + g_dirofs[best]);
    return true;
}

/* -------------------------------------------------------------- boats --- */
/* a shore tile near `target` with the same owner, on a navigable sea */
static tile_t landing_near(tile_t target, uint8_t *comp)
{
    uint8_t own = OWNER_OF(target), r;
    int16_t x, y, cx = (int16_t)tx(target), cy = (int16_t)ty(target);
    tile_t best = NO_TILE;
    uint16_t bd = 0xFFFF;
    for (r = 0; r <= 12; r += 3) {
        for (y = cy - r; y <= cy + r; y++) {
            if (y < 1 || y >= (int16_t)g_H - 1) continue;
            for (x = cx - r; x <= cx + r; x++) {
                tile_t t;
                uint8_t c;
                uint16_t d;
                if (x < 1 || x >= (int16_t)g_W - 1) continue;
                t = TILE(x, y);
                if (!IS_LAND(g_map[t]) || OWNER_OF(t) != own) continue;
                c = shore_comp(t);
                if (!c) continue;
                d = (uint16_t)((x - cx) * (x - cx) + (y - cy) * (y - cy));
                if (d < bd) {
                    bd = d;
                    best = t;
                    *comp = c;
                }
            }
        }
        if (best != NO_TILE) break;
    }
    return best;
}

bool launch_boat(uint8_t p, tile_t target, uint32_t troops)
{
    player_t *pl = &g_pl[p];
    uint8_t comp = 0, def, slot;
    tile_t land, src, w0, w1;
    unit_t *u;
    if (!IS_LAND(g_map[target])) return false;
    def = OWNER_OF(target);
    if (def == p || (def && ALLIED(p, def))) return false;
    if (pl->boats >= 3) {
        if (p == HUMAN) ui_event(EV_BUSY, 0, 0);
        return false;
    }
    if (troops > pl->troops) troops = pl->troops;
    if (troops < 100) {
        if (p == HUMAN) ui_event(EV_NOTROOPS, 0, 0);
        return false;
    }
    land = landing_near(target, &comp);
    src = land == NO_TILE ? NO_TILE : nearest_own_shore(p, land, comp);
    if (src == NO_TILE) {
        if (p == HUMAN) ui_event(EV_NOROUTE, 0, 0);
        return false;
    }
    w0 = water_next_to(src, comp);
    w1 = water_next_to(land, comp);
    slot = path_find(w0, w1);
    if (slot == 0xFF) {
        if (p == HUMAN) ui_event(EV_NOROUTE, 0, 0);
        return false;
    }
    u = new_unit(U_BOAT, p, w0);
    if (!u) {
        path_release(slot);
        return false;
    }
    u->dst = w1;
    u->src = land;
    u->aux = def;
    u->path = slot;
    u->troops = troops;
    u->timer = ship_delay();
    pl->troops -= troops;
    pl->boats++;
    return true;
}

void boat_tick(unit_t *u)
{
    uint8_t p = u->owner, o, r;
    uint32_t troops = u->troops;
    tile_t land = u->src;
    if (--u->timer) return;
    u->timer = ship_delay();
    r = ship_advance(u);
    if (!r) return;
    del_unit(u);
    if (!g_pl[p].alive) return;
    if (r == 2) {
        add_troops(p, troops);
        return;
    }
    o = OWNER_OF(land);
    if (o == p || (o && ALLIED(p, o))) {
        add_troops(p, troops);
        return;
    }
    if (o == HUMAN) ui_event(EV_BOAT_LAND, p, 0);
    conquer(land, p);
    if (!g_pl[p].alive) return;
    if (o && !g_pl[o].alive) o = 0;
    if (!attack_start(p, o, troops, false)) add_troops(p, troops);
}

/* -------------------------------------------------------------- trade --- */
/* OpenFront: 75000 / (1 + e^(-0.03 (d - 300))) + 50 d, d in original tiles */
static uint32_t trade_gold(uint16_t tiles)
{
    uint32_t d = ((uint32_t)tiles * g_L16) >> 4, s256;
    int32_t k = ((int32_t)300 - (int32_t)d) * 11; /* Q8 exponent of 2 */
    if (k < -2048)
        s256 = 256;
    else if (k > 4096)
        s256 = 0;
    else
        s256 = 65536UL / (256 + fx_exp2((int16_t)(k + 2048)));
    return ((75000UL * s256) >> 8) + 50 * d;
}

void port_tick(uint8_t i)
{
    unit_t *port = &g_un[i], *dst = 0, *u;
    uint8_t p = port->owner, comp, n = 0, k, l, slot;
    uint8_t cand[48];
    uint16_t cdist[48], lv, f, boost, nb, close;
    uint32_t rate, total = 0, roll;
    tile_t w0, w1;

    if (((uint8_t)g_tick + i) & 7) return;
    if (g_nTrade >= TRADE_CAP) return;

    /* spawn odds per port level, with OpenFront's small-economy boost and
     * pity timer; checked every 8 ticks */
    lv = levels_of(p, U_PORT);
    f = lv >= 6 ? 0 : (uint16_t)((6 - lv) * 256 / 5);
    boost = (uint16_t)(256 + ((128u * f) >> 8) / (lv ? lv : 1));
    rate = (125UL * 65536UL) / ((uint32_t)boost * boost);
    rate /= (uint32_t)port->aux2 + 1;
    rate = rate * TRADE_CAP / (TRADE_CAP - g_nTrade);
    if (!rate) rate = 1;
    for (l = 0; l < port->level; l++) {
        if (rnd_n((uint16_t)(rate > 60000 ? 60000 : rate)) == 0) break;
        if (port->aux2 < 250) port->aux2++;
    }
    if (l == port->level) return;
    port->aux2 = 0;

    comp = shore_comp(port->pos);
    for (k = 0; k < g_unHi && n < 48; k++) {
        unit_t *c = &g_un[k];
        if (c->type != U_PORT || !BUILT(c) || c->owner == p || !g_pl[c->owner].alive) continue;
        if (!can_trade(p, c->owner) || water_next_to(c->pos, comp) == NO_TILE) continue;
        cand[n] = k;
        cdist[n] = mdist(port->pos, c->pos);
        n++;
    }
    if (!n) return;
    nb = n / 3 < 4 ? 4 : n / 3;
    close = (uint16_t)((300u * 16) / g_L16); /* closer than this pays badly */
    for (k = 0; k < n; k++) {
        uint8_t j, rank = 0, mult = 1;
        for (j = 0; j < n; j++)
            if (cdist[j] < cdist[k]) rank++;
        if (cdist[k] >= close) {
            if (rank < nb) mult++;
            if (ALLIED(p, g_un[cand[k]].owner)) mult++;
        }
        cdist[k] = (uint16_t)(g_un[cand[k]].level * mult); /* now the weight */
        total += cdist[k];
    }
    roll = rnd_n((uint16_t)total);
    for (k = 0; k < n; k++) {
        if (roll < cdist[k]) {
            dst = &g_un[cand[k]];
            break;
        }
        roll -= cdist[k];
    }
    if (!dst) return;

    w0 = water_next_to(port->pos, comp);
    w1 = water_next_to(dst->pos, comp);
    slot = path_find(w0, w1);
    if (slot == 0xFF) return;
    u = new_unit(U_TRADE, p, w0);
    if (!u) {
        path_release(slot);
        return;
    }
    u->dst = w1;
    u->tgt = dst->pos;
    u->src = port->pos;
    u->path = slot;
    u->aux = dst->owner;
    u->orig = p;
    u->timer = ship_delay();
}

/* send a captured trade ship to the captor's nearest port */
static bool trade_redirect(unit_t *u)
{
    uint8_t comp = g_map[u->pos] & T_COMP;
    unit_t *port = port_on(u->owner, comp, u->pos);
    if (!port) return false;
    path_release(u->path);
    u->path = 0xFF;
    u->timer2 = 0;
    u->tgt = port->pos;
    u->dst = water_next_to(port->pos, comp);
    u->aux = u->owner;
    return u->dst != NO_TILE;
}

void trade_tick(unit_t *u)
{
    uint32_t g;
    uint8_t a = u->orig, b = u->aux, r;
    int16_t k;
    bool captured = u->owner != u->orig;
    if (--u->timer) return;
    u->timer = ship_delay();
    k = unit_at(u->tgt);
    if (k < 0 || g_un[k].type != U_PORT || g_un[k].owner != b || !g_pl[b].alive) {
        if (!captured || !trade_redirect(u)) {
            del_unit(u);
            return;
        }
    } else if (!captured && (b == a || !can_trade(a, b))) {
        del_unit(u);
        return;
    }
    r = ship_advance(u);
    if (!r) return;
    if (r == 2) {
        del_unit(u);
        return;
    }
    g = gold_mul(trade_gold(u->dist));
    del_unit(u);
    if (captured) {
        if (g_pl[u->owner].alive) g_pl[u->owner].gold += g;
        if (u->owner == HUMAN) ui_event(EV_TRADE, (uint8_t)(g / 1000 > 255 ? 255 : g / 1000), a);
        return;
    }
    if (g_pl[a].alive) g_pl[a].gold += g;
    if (g_pl[b].alive) g_pl[b].gold += g;
    if (a == HUMAN || b == HUMAN)
        ui_event(EV_TRADE, (uint8_t)(g / 1000 > 255 ? 255 : g / 1000), a == HUMAN ? b : a);
}

/* ----------------------------------------------------------- warships --- */
bool warship_spawn(uint8_t p, tile_t at)
{
    uint8_t comp = g_map[at] & T_COMP;
    unit_t *port = port_on(p, comp, at), *u;
    tile_t w;
    if (!port) return false;
    w = water_next_to(port->pos, comp);
    if (w == NO_TILE) return false;
    u = new_unit(U_WARSHIP, p, w);
    if (!u) return false;
    u->level = 0; /* veterancy */
    u->hp = WS_MAXHP(u);
    u->dst = at;
    u->tgt = at;
    u->orig = WS_PATROL;
    u->timer = 1;
    return true;
}

void warship_order(uint8_t idx, tile_t to)
{
    unit_t *u = &g_un[idx];
    if (u->type != U_WARSHIP || IS_LAND(g_map[to]) ||
        (g_map[to] & T_COMP) != (g_map[u->pos] & T_COMP))
        return;
    path_release(u->path);
    u->path = 0xFF;
    u->timer2 = 0;
    u->dst = to;
    u->tgt = to;
    u->aux = 0;
    u->orig = WS_PATROL;
}

static bool hostile(uint8_t p, uint8_t o) { return o != p && !ALLIED(p, o); }

static uint8_t find_target(unit_t *u, bool retreating)
{
    uint8_t i, best = 0xFF, bestCls = 9, p = u->owner;
    uint8_t r = orig2tiles(130);
    uint16_t bd = 0xFFFF;
    for (i = 0; i < g_unHi; i++) {
        unit_t *o = &g_un[i];
        uint8_t cls;
        uint16_t d;
        if (!IS_SHIP(o->type) || !hostile(p, o->owner) || o == u) continue;
        if (o->type == U_BOAT)
            cls = o->hp ? 3 : 0; /* already being shot at: low priority */
        else if (o->type == U_WARSHIP)
            cls = 1;
        else {
            if (retreating || o->aux == p || ALLIED(p, o->aux)) continue;
            cls = 2;
        }
        if (cls > bestCls) continue;
        d = mdist(o->pos, u->pos);
        if (d > 2 * r || !near(o->pos, u->pos, r)) continue;
        if (cls < bestCls || d < bd) {
            bestCls = cls;
            bd = d;
            best = i;
        }
    }
    return best;
}

static void fire(unit_t *u, uint8_t self, uint8_t target)
{
    unit_t *t = &g_un[target], *s;
    uint16_t dmg = (uint16_t)(20 + rnd_n(11)) * (100 + 20 * u->level) / 100;
    s = new_unit(U_SHELL, u->owner, u->pos);
    if (!s) return;
    s->aux = target;
    s->aux2 = self;
    s->level = (uint8_t)dmg;
    s->timer = 40;
    if (t->type == U_BOAT)
        t->hp = 1;   /* one shell sinks a boat: no reload needed */
    else
        u->aux2 = 20; /* reload */
}

/* warships: troops = kills, src = trade ships captured */
static void veterancy(unit_t *ws)
{
    if (ws->level < 3 && ws->troops >= (uint32_t)(5 * (ws->level + 1))) {
        ws->level++;
        ws->hp = (uint8_t)(ws->hp + 20);
    }
}

static void capture_trade(unit_t *ws, unit_t *t)
{
    uint8_t old = t->owner;
    t->owner = ws->owner;
    if (!trade_redirect(t)) {
        del_unit(t);
        return;
    }
    rel_add(old, ws->owner, -10);
    if (old == HUMAN) ui_event(EV_SHIP_LOST, ws->owner, 0);
    if (ws->owner == HUMAN) ui_event(EV_CAPTURED, 0, old);
    if (++ws->src >= 10) {
        ws->src = 0;
        ws->troops += 5;
        veterancy(ws);
    }
}

static void patrol_step(unit_t *u)
{
    uint8_t r;
    if (u->tgt == NO_TILE || u->pos == u->tgt) {
        uint8_t comp = g_map[u->pos] & T_COMP;
        path_release(u->path);
        u->path = 0xFF;
        u->timer2 = 0;
        u->tgt = random_water_near(u->dst, orig2tiles(100) / 2 + 2, comp);
        return;
    }
    if (u->path == 0xFF) {
        u->path = path_find(u->pos, u->tgt);
        u->timer2 = 0;
        if (u->path == 0xFF) {
            u->tgt = NO_TILE;
            return;
        }
    }
    {
        tile_t keep = u->dst;
        u->dst = u->tgt;
        r = ship_advance(u);
        u->dst = keep;
    }
    if (r == 2) u->tgt = NO_TILE;
}

void warship_tick(uint8_t i)
{
    unit_t *u = &g_un[i], *tg = 0;
    uint8_t p = u->owner, comp = g_map[u->pos] & T_COMP, maxhp = (uint8_t)WS_MAXHP(u);
    bool retreating = u->orig == WS_RETREAT;

    if (u->aux2) u->aux2--;

    /* passive repair near an own port */
    if ((g_tick & 15) == 0 && u->hp < maxhp) {
        unit_t *port = port_on(p, comp, u->pos);
        if (port && near(port->pos, u->pos, orig2tiles(150))) u->hp++;
    }

    if (u->orig == WS_DOCKED) {
        unit_t *port = port_on(p, comp, u->pos);
        if (!port || mdist(port->pos, u->pos) > 2) {
            u->orig = WS_PATROL;
        } else if (g_tick & 1) {
            uint16_t h = u->hp + 1 + port->level;
            u->hp = (uint8_t)(h > maxhp ? maxhp : h);
        }
        if (u->hp >= maxhp) {
            u->orig = WS_PATROL;
            u->tgt = NO_TILE;
        }
        return;
    }

    /* decide to go home for repairs */
    if (!retreating && (uint16_t)u->hp * 4 <= (uint16_t)maxhp * 3) {
        unit_t *port = port_on(p, comp, u->pos);
        if (port) {
            path_release(u->path);
            u->path = 0xFF;
            u->timer2 = 0;
            u->tgt = water_next_to(port->pos, comp);
            u->orig = WS_RETREAT;
            retreating = true;
        }
    }

    if (((uint8_t)g_tick + i) & 3) {
        if (u->aux) {
            tg = &g_un[u->aux - 1];
            if (!IS_SHIP(tg->type) || !hostile(p, tg->owner)) {
                u->aux = 0;
                tg = 0;
            }
        }
    } else {
        uint8_t t = find_target(u, retreating);
        u->aux = t == 0xFF ? 0 : t + 1;
        if (u->aux) tg = &g_un[t];
    }

    /* shoot boats and warships in range */
    if (tg && tg->type != U_TRADE && !u->aux2 && !(tg->type == U_BOAT && tg->hp))
        fire(u, i, (uint8_t)(u->aux - 1));

    if (--u->timer) return;

    if (tg && tg->type == U_TRADE && !retreating) {
        /* chase: twice as fast, then capture alongside */
        u->timer = ship_delay() > 2 ? ship_delay() / 2 : 1;
        if (mdist(u->pos, tg->pos) <= 1) {
            capture_trade(u, tg);
            u->aux = 0;
            return;
        }
        path_release(u->path);
        u->path = 0xFF;
        if (!ship_greedy(u, tg->pos)) u->aux = 0;
        u->tgt = NO_TILE;
        return;
    }
    u->timer = ship_delay();

    if (retreating) {
        if (u->tgt == NO_TILE || u->pos == u->tgt) {
            u->orig = u->tgt == NO_TILE ? WS_PATROL : WS_DOCKED;
            u->tgt = NO_TILE;
            return;
        }
    }
    patrol_step(u);
    if (retreating && u->tgt == NO_TILE) u->orig = WS_PATROL;
}

void shell_tick(unit_t *u)
{
    unit_t *t = &g_un[u->aux];
    uint8_t k;
    if (!IS_SHIP(t->type) || t->owner == u->owner || !u->timer--) {
        u->type = U_NONE;
        return;
    }
    for (k = 0; k < 2; k++) {
        int16_t x = (int16_t)tx(u->pos), y = (int16_t)ty(u->pos);
        int16_t gx = (int16_t)tx(t->pos), gy = (int16_t)ty(t->pos);
        if (x == gx && y == gy) break;
        x += gx > x ? 1 : (gx < x ? -1 : 0);
        y += gy > y ? 1 : (gy < y ? -1 : 0);
        u->pos = TILE(x, y);
    }
    if (mdist(u->pos, t->pos) > 1) return;
    /* hit */
    u->type = U_NONE;
    if (t->type == U_WARSHIP && t->hp > u->level) {
        t->hp -= u->level;
        return;
    }
    {
        unit_t *ws = &g_un[u->aux2];
        uint8_t victim = t->owner, kind = t->type;
        if (victim == HUMAN) ui_event(EV_LOST_UNIT, kind, u->owner);
        if (u->owner == HUMAN) ui_event(EV_SUNK, kind, victim);
        rel_add(victim, u->owner, -10);
        del_unit(t);
        if (ws->type == U_WARSHIP && ws->owner == u->owner) {
            ws->troops++;
            veterancy(ws);
        }
    }
}
