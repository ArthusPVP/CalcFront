/* computer players: tribes (passive grey bots) and nations (smart AI) */
#include "sim.h"

/* in steps of 8 ticks: a nation acts every 80 / 64 / 56 / 40 ticks, the middle
 * of OpenFront's ranges (65-100, 55-70, 45-60, 30-50) */
static const uint8_t thinkEvery[4] = {9, 7, 6, 4};
/* % of the troop cap a nation keeps before hitting back at an attacker */
static const uint8_t fightMin[4] = {55, 35, 15, 15};

void ai_init_player(uint8_t p)
{
    g_pl[p].aiWait = (uint8_t)(2 + rnd_n(8));
    g_pl[p].aiFlags = (uint8_t)rnd();
}

static tile_t rand_tile_of(uint8_t p, uint8_t tries)
{
    player_t *pl = &g_pl[p];
    uint16_t w = (uint16_t)(pl->maxx - pl->minx + 1), h = (uint16_t)(pl->maxy - pl->miny + 1);
    if (!pl->tiles) return NO_TILE;
    while (tries--) {
        tile_t t = rnd_tile(pl->minx, w, pl->miny, h);
        uint8_t b = g_map[t];
        if (IS_LAND(b) && (b & T_OWNER) == p) return t;
    }
    return NO_TILE;
}

/* own land tile within r of c */
static tile_t own_tile_near(uint8_t p, tile_t c, uint8_t r, uint8_t tries)
{
    int16_t cx = (int16_t)tx(c), cy = (int16_t)ty(c);
    while (tries--) {
        int16_t x = (int16_t)(cx + rnd_n(2 * r + 1) - r), y = (int16_t)(cy + rnd_n(2 * r + 1) - r);
        tile_t t;
        if (x < 1 || y < 1 || x >= (int16_t)g_W - 1 || y >= (int16_t)g_H - 1) continue;
        t = TILE(x, y);
        if (IS_LAND(g_map[t]) && OWNER_OF(t) == p) return t;
    }
    return NO_TILE;
}

static uint8_t pick_neighbor(uint8_t p, bool tribeStyle)
{
    player_t *pl = &g_pl[p];
    uint32_t mask = pl->nbrs & ~pl->allies;
    uint8_t i, best = 0;
    uint32_t bestScore = 0xFFFFFFFF;
    if (!mask) return 0;
    for (i = 1; i <= g_nplayers; i++) {
        player_t *o = &g_pl[i];
        uint32_t score;
        if (!MASKBIT(mask, i) || !o->alive || i == p) continue;
        if (tribeStyle) {
            if (o->type != PT_TRIBE && (rnd() & 1)) continue;
            score = rnd();
        } else {
            score = o->troops + 1;
            if (o->type == PT_TRIBE) score >>= 1;
            if (o->traitor) score >>= 1;
            if (pl->rel[i] < -50) score -= score >> 2; /* hostile */
            if (pl->rel[i] > 30) score += score >> 1;
            if (g_pl[HUMAN].alive && ALLIED(p, HUMAN) && g_pl[HUMAN].target == i) score >>= 1;
            if (i == pl->aiEnemy) score >>= 1;
        }
        if (score < bestScore) {
            bestScore = score;
            best = i;
        }
    }
    return best;
}

static void answer_requests(uint8_t p)
{
    player_t *pl = &g_pl[p];
    uint8_t i;
    if (!pl->reqFrom) return;
    for (i = 1; i <= g_nplayers; i++) {
        bool yes;
        if (!MASKBIT(pl->reqFrom, i)) continue;
        if (pl->type == PT_TRIBE)
            yes = true;
        else {
            yes = pl->rel[i] > -15 && !attacking(i, p) && !g_pl[i].traitor;
            /* a strong nation would rather eat a weak neighbour */
            if (yes && g_cfg.difficulty >= DIFF_HARD && MASKBIT(pl->nbrs, i) &&
                g_pl[i].troops < pl->troops / 3)
                yes = false;
            if (yes && attacking(p, i) && pl->troops > g_pl[i].troops) yes = false;
        }
        ally_answer(i, p, yes);
    }
}

static void tribe_think(uint8_t p)
{
    player_t *pl = &g_pl[p];
    uint8_t from, trigger = (uint8_t)(50 + (pl->aiFlags & 31));
    uint8_t target;
    answer_requests(p);
    if (pl->troops < (pl->maxTroops / 100) * trigger) return;
    if (incoming_troops(p, &from) && from && !ALLIED(p, from)) {
        if (attack_start(p, from, pl->troops / 3, true)) return;
    }
    if (pl->nbrTN) {
        if (attack_start(p, 0, pl->troops / 5, true)) return;
        pl->nbrTN = 0;
    }
    if ((pl->aiFlags & 64) == 0) {
        /* refresh the neighbour list once in a while */
        scan_player(p);
        pl->aiFlags |= 64;
        if (pl->nbrTN) return;
    } else if ((rnd() & 7) == 0)
        pl->aiFlags &= (uint8_t)~64;
    target = pick_neighbor(p, true);
    if (target) attack_start(p, target, pl->troops / 3, true);
}

/* ---------------------------------------------------------------- nation --- */
static bool try_build_at(uint8_t p, uint8_t type, tile_t t)
{
    if (t == NO_TILE || has_struct(t) || !can_build(p, type, t)) return false;
    return build(p, type, t);
}

static bool try_build(uint8_t p, uint8_t type)
{
    uint8_t tries = 12;
    if (g_pl[p].gold < unit_cost(p, type)) return false;
    while (tries--) {
        tile_t t = rand_tile_of(p, 6);
        if (t == NO_TILE) return false;
        if (type == U_PORT && !is_shore(t)) {
            /* look along the coast nearby */
            uint8_t k;
            for (k = 0; k < 4; k++) {
                tile_t n = own_tile_near(p, t, 4, 3);
                if (n != NO_TILE && is_shore(n)) {
                    t = n;
                    break;
                }
            }
        }
        if (try_build_at(p, type, t)) return true;
    }
    return false;
}

static bool try_upgrade(uint8_t p, uint8_t type)
{
    uint8_t i;
    if (g_pl[p].gold < unit_cost(p, type)) return false;
    for (i = 0; i < g_unHi; i++)
        if (g_un[i].type == type && g_un[i].owner == p && can_build(p, type, g_un[i].pos))
            return build(p, type, g_un[i].pos);
    return false;
}

/* a factory next to existing cities/ports, so it becomes a rail hub */
static bool try_factory(uint8_t p)
{
    uint8_t i, tries;
    if (g_pl[p].gold < unit_cost(p, U_FACTORY)) return false;
    for (i = 0; i < g_unHi; i++) {
        unit_t *u = &g_un[i];
        if ((u->type != U_CITY && u->type != U_PORT) || u->owner != p || (u->aux & 1)) continue;
        for (tries = 0; tries < 6; tries++)
            if (try_build_at(p, U_FACTORY, own_tile_near(p, u->pos, orig2tiles(60), 4))) return true;
    }
    return false;
}

/* whose ports a nation's warships go raiding */
static bool hostile_trade_target(uint8_t p, uint8_t o)
{
    return o != p && !ALLIED(p, o) && (g_cfg.difficulty >= DIFF_HARD || g_pl[p].rel[o] < 0);
}

static void nation_navy(uint8_t p)
{
    player_t *pl = &g_pl[p];
    uint8_t i, enemy = pl->aiEnemy;
    for (i = 0; i < g_unHi; i++) {
        unit_t *u = &g_un[i];
        uint8_t comp, k;
        tile_t goal = NO_TILE;
        if (u->type != U_WARSHIP || u->owner != p || u->orig != WS_PATROL) continue;
        if (rnd() & 3) continue;
        comp = g_map[u->pos] & T_COMP;
        /* hunt near an enemy port, else guard one of ours */
        if (enemy && g_pl[enemy].alive) {
            unit_t *ep = port_on(enemy, comp, u->pos);
            if (ep) goal = water_next_to(ep->pos, comp);
        }
        if (goal == NO_TILE) {
            for (k = 0; k < 8 && goal == NO_TILE; k++) {
                unit_t *c = &g_un[rnd_n(g_unHi)];
                if (c->type == U_PORT && BUILT(c) && hostile_trade_target(p, c->owner))
                    goal = water_next_to(c->pos, comp);
            }
        }
        if (goal == NO_TILE) {
            unit_t *own = port_on(p, comp, u->pos);
            if (own) goal = random_water_near(own->pos, 8, comp);
        }
        if (goal != NO_TILE) warship_order(i, goal);
    }
}

static void nation_build(uint8_t p)
{
    player_t *pl = &g_pl[p];
    uint8_t diff = g_cfg.difficulty;
    uint8_t structs = 0, ports, cities, factories, ships, i;
    uint8_t maxStructs = (uint8_t)(3 + pl->tiles / 40);
    if (maxStructs > 24) maxStructs = 24;
    for (i = 0; i < g_unHi; i++)
        if (IS_STRUCT(g_un[i].type) && g_un[i].owner == p) structs++;
    ports = count_units(p, U_PORT);
    cities = count_units(p, U_CITY);
    factories = count_units(p, U_FACTORY);
    ships = count_units(p, U_WARSHIP);

    if (pl->gold < 100000) return;
    /* ports first: trade is the main income */
    if (ports < 1 + pl->tiles / 300 && ports < 4 && structs < maxStructs) {
        if (try_build(p, U_PORT)) return;
    }
    if (cities < 1 + pl->tiles / 150 && structs < maxStructs) {
        if (try_build(p, U_CITY)) return;
    }
    if (diff >= DIFF_MEDIUM && cities + ports >= 2 && factories < 1 + (cities + ports) / 4 &&
        structs < maxStructs) {
        if (try_factory(p)) return;
    }
    if (diff >= DIFF_MEDIUM && pl->aiEnemy && count_units(p, U_POST) < 2 &&
        g_pl[pl->aiEnemy].troops > pl->troops && structs < maxStructs) {
        if (try_build(p, U_POST)) return;
    }
    if (diff >= DIFF_MEDIUM && !count_units(p, U_SILO) && pl->gold > 1400000) {
        if (try_build(p, U_SILO)) return;
    }
    if (diff >= DIFF_HARD && !count_units(p, U_SAM) && pl->gold > 2500000) {
        if (try_build(p, U_SAM)) return;
    }
    if (ports && ships < 1 + diff / 2 + (pl->tiles > 600) && pl->gold > unit_cost(p, U_WARSHIP) * 2) {
        unit_t *port = port_on(p, 0, TILE(pl->minx, pl->miny));
        if (port) {
            tile_t w = water_next_to(port->pos, shore_comp(port->pos));
            if (w != NO_TILE && build(p, U_WARSHIP, w)) return;
        }
    }
    /* upgrades when rich: ports, then cities and factories */
    if (pl->gold > 600000 && (rnd() & 1)) {
        if (try_upgrade(p, U_PORT)) return;
    }
    if (pl->troops > pl->maxTroops / 2 && (rnd() & 1)) {
        if (try_upgrade(p, U_CITY)) return;
    }
    if (diff >= DIFF_HARD && pl->gold > 900000 && (rnd() & 3) == 0) try_upgrade(p, U_FACTORY);
}

static uint8_t strongest_enemy(uint8_t p)
{
    uint8_t i, best = 0;
    uint16_t bt = 0;
    for (i = 1; i <= g_nplayers; i++) {
        player_t *o = &g_pl[i];
        if (i == p || !o->alive || ALLIED(p, i)) continue;
        if (o->tiles > bt) {
            bt = o->tiles;
            best = i;
        }
    }
    return best;
}

static void nation_nuke(uint8_t p)
{
    player_t *pl = &g_pl[p];
    uint8_t victim = pl->aiEnemy, type = U_ATOM, i;
    tile_t target = NO_TILE;
    if (pl->gold < 750000 + 200000 || find_silo(p) < 0) return;
    if (!victim || !g_pl[victim].alive || ALLIED(p, victim)) {
        victim = strongest_enemy(p);
        /* only nuke a bystander if it is running away with the game */
        if (!victim || land_pct10(victim) < 250) return;
    }
    if (g_pl[victim].tiles < 30) return;
    if (pl->gold > 5500000 && g_pl[victim].tiles > 400) type = U_HYDRO;
    if (pl->gold > unit_cost(p, U_MIRV) + 1000000 && land_pct10(victim) > 400) type = U_MIRV;
    for (i = 0; i < g_unHi; i++) {
        unit_t *u = &g_un[i];
        if (IS_STRUCT(u->type) && u->owner == victim && (rnd() & 1)) {
            target = u->pos;
            break;
        }
    }
    if (target == NO_TILE) target = rand_tile_of(victim, 30);
    if (target != NO_TILE) launch_nuke(p, type, target);
}

/* land tile on a sea in `seas` (bit c = water body c) */
static bool shore_on(tile_t t, uint8_t seas)
{
    uint8_t c = shore_comp(t);
    return c && (seas & (uint8_t)(1 << c));
}

static void nation_boat(uint8_t p)
{
    player_t *pl = &g_pl[p];
    uint8_t tries = 24, i, victim = 0, seas;
    tile_t best = NO_TILE;
    uint16_t bd = 0xFFFF;
    uint32_t vt = 0xFFFFFFFF, limit;
    tile_t home = TILE((pl->minx + pl->maxx) / 2, (pl->miny + pl->maxy) / 2);
    if (pl->boats >= 1 + (g_cfg.difficulty >= DIFF_HARD)) return;
    seas = coast_bodies(p); /* only aim at coasts on seas we can sail */
    if (!seas) return;

    /* an overseas rival: the weakest one we can take on */
    limit = pl->troops >= pl->maxTroops - pl->maxTroops / 10 ? pl->troops : pl->troops / 2;
    for (i = 1; i <= g_nplayers; i++) {
        player_t *o = &g_pl[i];
        if (i == p || !o->alive || ALLIED(p, i) || o->troops > limit) continue;
        if (o->troops < vt) {
            vt = o->troops;
            victim = i;
        }
    }
    if (victim && (rnd() & 1)) {
        player_t *o = &g_pl[victim];
        for (i = 0; i < 40; i++) {
            tile_t t = rnd_tile(o->minx, (uint16_t)(o->maxx - o->minx + 1), o->miny, (uint16_t)(o->maxy - o->miny + 1));
            if (IS_LAND(g_map[t]) && OWNER_OF(t) == victim && shore_on(t, seas)) {
                if (launch_boat(p, t, pl->troops / 3)) return;
                break;
            }
        }
    }
    while (tries--) {
        tile_t t = random_land();
        uint8_t o;
        uint16_t d;
        if (t == NO_TILE || !shore_on(t, seas)) continue;
        o = OWNER_OF(t);
        if (o == p || (o && ALLIED(p, o))) continue;
        if (o && g_pl[o].troops > pl->troops / 2) continue;
        d = mdist(t, home);
        if (!o) d >>= 1; /* free land is the better prize */
        if (d < bd) {
            bd = d;
            best = t;
        }
    }
    if (best != NO_TILE) launch_boat(p, best, pl->troops / 4);
}

static void nation_think(uint8_t p)
{
    player_t *pl = &g_pl[p];
    uint8_t diff = g_cfg.difficulty, from, target;
    uint32_t inc, pct;

    pl->aiFlags++;
    pl->aiTurn++;
    if ((pl->aiFlags & 15) == 0) scan_player(p);
    answer_requests(p);

    /* forget an enemy once the war is over and the grudge has faded */
    if (pl->aiEnemy && (!g_pl[pl->aiEnemy].alive || ALLIED(p, pl->aiEnemy) ||
                        (pl->rel[pl->aiEnemy] >= -50 && !attacking(p, pl->aiEnemy) &&
                         !attacking(pl->aiEnemy, p))))
        pl->aiEnemy = 0;
    inc = incoming_troops(p, &from);
    if (inc && from) pl->aiEnemy = from;
    {
        /* trade: stop with the players we hate; on Easy and Medium start
         * again once the grudge has faded to neutral (OpenFront) */
        uint8_t i;
        for (i = 1; i <= g_nplayers; i++) {
            if (pl->rel[i] < -50)
                MASKSET(pl->embargo, i);
            else if (pl->rel[i] >= 0 && diff <= DIFF_MEDIUM)
                MASKCLR(pl->embargo, i);
        }
    }

    PROF(8, nation_build(p));
    if ((pl->aiTurn & 3) == 0) PROF(10, nation_navy(p));
    if (diff >= DIFF_MEDIUM) PROF(10, nation_nuke(p));

    pct = pl->troops / ((pl->maxTroops / 100) + 1);

    /* ask for help when in trouble */
    if (inc > pl->troops / 2 && (rnd() & 3) == 0) {
        uint8_t i;
        for (i = 1; i <= g_nplayers; i++)
            if (i != p && i != from && g_pl[i].alive && g_pl[i].type != PT_TRIBE &&
                !ALLIED(p, i) && pl->rel[i] >= 0 && MASKBIT(pl->nbrs, i)) {
                ally_request(p, i);
                break;
            }
    }

    /* free land first */
    if (pl->nbrTN) {
        if (pct >= 25 && !attacking(p, 0)) {
            if (attack_start(p, 0, pl->troops / (diff >= DIFF_HARD ? 3 : 4), true)) return;
            pl->nbrTN = 0;
        } else if (attacking(p, 0))
            return;
    }

    /* fight back, once saved up as OpenFront's nations do: Easy only past
     * its attack trigger, Medium past its reserve, Hard and up at once */
    if (inc && from && !ALLIED(p, from) && pct >= fightMin[diff]) {
        if (attack_start(p, from, pl->troops / 2, true)) return;
    }

    if (pct < (uint32_t)(70 - diff * 8)) return;

    target = pick_neighbor(p, false);
    /* spare the player on the lower difficulties, as OpenFront does: on Easy
     * attack them one time in 4, on Medium three times in 4 */
    if (target == HUMAN && diff <= DIFF_MEDIUM && (rnd_n(4) == 0) == (diff == DIFF_MEDIUM)) return;
    if (target) {
        player_t *o = &g_pl[target];
        if (o->type == PT_TRIBE || pl->troops > (o->troops / 4) * 3 ||
            (target == pl->aiEnemy && o->troops / 3 < pl->troops)) {
            pl->aiEnemy = target;
            if (attack_start(p, target, (pl->troops / 10) * (4 + diff), true)) return;
        }
    }
    /* nothing to do on land: go overseas */
    if ((!pl->nbrs || !target) && !pl->nbrTN && pct >= 60) PROF(9, nation_boat(p));

    /* occasional treachery on the higher difficulties */
    if (diff >= DIFF_HARD && !target && pl->allies && (rnd() & 15) == 0) {
        uint8_t i;
        for (i = 1; i <= g_nplayers; i++)
            if (ALLIED(p, i) && MASKBIT(pl->nbrs, i) && g_pl[i].troops < pl->troops / 3) {
                ally_break(p, i);
                pl->aiEnemy = i;
                break;
            }
    }
}

void ai_tick(void)
{
    uint8_t p, slot = (uint8_t)(g_tick & 7);
    for (p = 2; p <= g_nplayers; p++) {
        player_t *pl = &g_pl[p];
        if (!pl->alive || (p & 7) != slot) continue;
        if (pl->aiWait) {
            pl->aiWait--;
            continue;
        }
        if (pl->type == PT_TRIBE) {
            pl->aiWait = (uint8_t)(5 + rnd_n(6));
            PROF(11, tribe_think(p));
        } else if (pl->type == PT_NATION) {
            pl->aiWait = thinkEvery[g_cfg.difficulty];
            nation_think(p);
        }
    }
}
