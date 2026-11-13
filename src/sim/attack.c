/* land attacks: frontier sampling, per-tile cost and losses */
#include "sim.h"

attack_t g_at[MAX_ATTACKS];

static const uint8_t terrMag[4] = {0, 80, 100, 120};
static const uint8_t terrCost2[4] = {0, 33, 40, 50}; /* tileCost * 2 */

FORCE_INLINE uint8_t own_nbrs(tile_t t, uint8_t p)
{
    uint8_t i, n = 0;
    for (i = 0; i < 4; i++) {
        uint8_t b = g_map[(tile_t)(t + g_dirofs[i])];
        if (IS_LAND(b) && (b & T_OWNER) == p) n++;
    }
    return n;
}

/* first tile from p to end (inclusive) that is land of player o, or 0. On
 * the calculator this is hand-written assembly (src/asm/scan.s). */
#ifdef HOST
static const uint8_t *next_owned(const uint8_t *p, const uint8_t *end, uint8_t o)
{
    for (; p <= end; p++) {
        uint8_t b = *p;
        if ((b & T_OWNER) == o && (b & T_TERR)) return p;
    }
    return 0;
}
#else
const uint8_t *next_owned(const uint8_t *p, const uint8_t *end, uint8_t o);
#endif

/* Sample the frontier (tiles of `def` touching `att`) into the ring.
 * With stopAtFirst it only answers "is there a border at all". */
static uint16_t scan_front(attack_t *a, uint8_t att, uint8_t def, bool stopAtFirst)
{
    player_t *pl = &g_pl[att];
    uint16_t x, y, x0, x1, y0, y1;
    uint16_t n = 0;
    int16_t W = (int16_t)g_W;
    if (!pl->tiles) return 0;
    x0 = pl->minx > 1 ? pl->minx - 1 : 1;
    y0 = pl->miny > 1 ? pl->miny - 1 : 1;
    x1 = pl->maxx + 2 < g_W ? pl->maxx + 1 : (uint16_t)(g_W - 2);
    y1 = pl->maxy + 2 < g_H ? pl->maxy + 1 : (uint16_t)(g_H - 2);
    if (def) {
        player_t *dp = &g_pl[def];
        if (!dp->tiles) return 0;
        if (dp->minx > x0) x0 = dp->minx;
        if (dp->miny > y0) y0 = dp->miny;
        if (dp->maxx < x1) x1 = dp->maxx;
        if (dp->maxy < y1) y1 = dp->maxy;
        if (x0 > x1 || y0 > y1) return 0;
    }
    for (y = y0;; y++) {
        tile_t t0 = g_rowStart[y];
        const uint8_t *p = g_map + t0 + x0, *end = g_map + t0 + x1;
        while ((p = next_owned(p, end, def))) {
            uint8_t nb;
            /* touches the attacker? (inline: this is the hottest loop) */
            nb = p[1];
            if (!(IS_LAND(nb) && (nb & T_OWNER) == att)) {
                nb = p[-1];
                if (!(IS_LAND(nb) && (nb & T_OWNER) == att)) {
                    nb = p[W];
                    if (!(IS_LAND(nb) && (nb & T_OWNER) == att)) {
                        nb = p[-W];
                        if (!(IS_LAND(nb) && (nb & T_OWNER) == att)) {
                            p++;
                            continue;
                        }
                    }
                }
            }
            if (stopAtFirst) return 1;
            {
                tile_t t = (tile_t)(p - g_map);
                if (n < RING)
                    a->ring[n] = t;
                else {
                    uint16_t j = rnd_n(n + 1);
                    if (j < RING) a->ring[j] = t;
                }
            }
            n++;
            p++;
        }
        if (y == y1) break;
    }
    if (a) {
        a->head = 0;
        a->count = n > RING ? RING : (uint8_t)n;
        a->border = n;
        /* shuffle so the front advances evenly */
        for (x = a->count; x > 1; x--) {
            uint8_t j = (uint8_t)rnd_n(x);
            tile_t t = a->ring[x - 1];
            a->ring[x - 1] = a->ring[j];
            a->ring[j] = t;
        }
    }
    return n;
}

bool borders(uint8_t att, uint8_t def) { return scan_front(0, att, def, true) != 0; }

/* loops over attacks walk a pointer: indexing a 274-byte record multiplies */
#define FOR_ATTACKS(a) for (a = g_at; a < g_at + MAX_ATTACKS; a++)

bool attacking(uint8_t att, uint8_t def)
{
    const attack_t *a;
    FOR_ATTACKS(a)
        if (a->active && a->att == att && a->def == def) return true;
    return false;
}

uint32_t incoming_troops(uint8_t p, uint8_t *from)
{
    const attack_t *a;
    uint32_t sum = 0, best = 0;
    if (from) *from = 0;
    FOR_ATTACKS(a) {
        if (a->active && a->def == p && !a->retreat) {
            sum += a->troops;
            if (a->troops >= best) {
                best = a->troops;
                if (from) *from = a->att;
            }
        }
    }
    return sum;
}

bool attack_start(uint8_t att, uint8_t def, uint32_t troops, bool removeFromOwner)
{
    attack_t *a = 0, *o;
    player_t *pl = &g_pl[att];
    if (att == def || !pl->alive) return false;
    if (def && (!g_pl[def].alive || ALLIED(att, def))) return false;
    if (removeFromOwner && troops > pl->troops) troops = pl->troops;
    if (troops < 10) return false;

    FOR_ATTACKS(o) {
        if (!o->active) {
            if (!a) a = o;
            continue;
        }
        if (o->att == att && o->def == def && !o->retreat) {
            if (removeFromOwner) pl->troops -= troops;
            o->troops += troops;
            return true;
        }
    }
    if (!a) return false;
    a->att = att;
    a->def = def;
    a->border = 0;
    {
        uint16_t found;
        PROF(0, found = scan_front(a, att, def, false));
        if (!found) return false;
    }
    if (removeFromOwner) pl->troops -= troops;

    /* opposing attack: the two stacks cancel out */
    if (def)
        FOR_ATTACKS(o) {
            if (o->active && o->att == def && o->def == att && !o->retreat) {
                if (o->troops > troops) {
                    o->troops -= troops;
                    return true;
                }
                troops -= o->troops;
                o->active = 0;
            }
        }
    a->active = 1;
    a->retreat = 0;
    a->troops = troops;
    a->budget = 0;
    a->rescan = 24;
    if (def) {
        rel_add(def, att, -40);
        if (def == HUMAN) ui_event(EV_ATTACKED, att, 0);
    }
    return true;
}

void attack_retreat(uint8_t att, uint8_t def)
{
    attack_t *a;
    FOR_ATTACKS(a)
        if (a->active && a->att == att && a->def == def) a->retreat = 1;
}

void attacks_drop_player(uint8_t p)
{
    attack_t *a;
    FOR_ATTACKS(a) {
        if (!a->active) continue;
        if (a->att == p)
            a->active = 0;
        else if (a->def == p) {
            add_troops(a->att, a->troops);
            a->active = 0;
        }
    }
}

static void end_attack(attack_t *a, uint8_t malusPct)
{
    uint32_t t = a->troops;
    if (malusPct) t -= (t / 100) * malusPct;
    if (g_pl[a->att].alive) add_troops(a->att, t);
    a->active = 0;
}

static void annex_all(uint8_t att, uint8_t def)
{
    player_t *dp = &g_pl[def];
    uint16_t x, y, x0 = dp->minx, x1 = dp->maxx, y0 = dp->miny, y1 = dp->maxy;
    for (y = y0;; y++) {
        for (x = x0;; x++) {
            tile_t t = TILE(x, y);
            uint8_t b = g_map[t];
            if (IS_LAND(b) && (b & T_OWNER) == def) conquer(t, att);
            if (x == x1) break;
        }
        if (y == y1) break;
    }
}

static void run_attack(attack_t *a)
{
    uint8_t att = a->att, def = a->def, pops = 0, taken = 0;
    player_t *ap = &g_pl[att], *dp = &g_pl[def];
    uint24_t B;
    /* per-attack values that do not change within a tick */
    uint16_t r8 = 0, lossRatio = 256, term = 0, bA = 256, bD = 256, bAs = 256;
    uint32_t dloss = 0, sc8 = 0;
    /* tile cost and loss depend only on terrain, fallout and a nearby defense
     * post within a tick: compute each combination once. On free land the
     * cost also depends on the attack's size, but only below 40000 troops. */
    uint32_t cCost[16], cLoss[16];
    uint8_t have[16];
    bool cacheOk;

    if (!ap->alive) {
        a->active = 0;
        return;
    }
    if (a->retreat) {
        end_attack(a, def ? 25 : 0);
        return;
    }
    if (def && (!dp->alive || ALLIED(att, def))) {
        end_attack(a, 0);
        return;
    }
    if (a->count == 0 || --a->rescan == 0) {
        uint16_t found;
        a->rescan = 24;
        PROF(0, found = scan_front(a, att, def, false));
        if (!found) {
            end_attack(a, 0);
            return;
        }
    }
    B = a->border;
    if (B < a->count) B = a->count;
    if (B > 600) B = 600;
    B += rnd_n(5);
    a->budget += B * 64;
    if (a->budget > 60000) a->budget = 60000;

    if (def) {
        uint32_t dt = dp->troops, q;
        uint16_t a8, b8;
        q = dt / ((a->troops >> 8) + 1); /* ~ ratio * 256 */
        r8 = q > 60000 ? 60000 : (uint16_t)q;
        lossRatio = r8 < 154 ? 154 : (r8 > 512 ? 512 : r8);
        bA = 256 - ((179 * ap->big8) >> 8);
        bD = 256 - ((77 * dp->big8) >> 8);
        bAs = 256 - ((187 * ap->big8) >> 8);
        dloss = dt / (dp->tiles ? dp->tiles : 1);
        term = (uint16_t)((((119u * bA) >> 8) * bD >> 8) * g_S + (dloss > 20000 ? 20000 : dloss));
        a8 = r8 < 210 ? 210 : (r8 > 1920 ? 1920 : r8);
        b8 = r8 / 20;
        b8 = b8 < 256 ? 256 : (b8 > 12800 ? 12800 : b8);
        sc8 = ((uint32_t)a8 * b8) >> 8;
    }
    memset(have, 0, sizeof have);
    cacheOk = def || a->troops >= 40000;

    while (taken < 24 && pops < 48 && a->count) {
        tile_t t = a->ring[a->head];
        uint8_t b = g_map[t], own, terr, i, key;
        uint16_t mag;
        uint24_t tc2;
        uint32_t cost, loss;
        pops++;
        if (!IS_LAND(b) || (b & T_OWNER) != def || !(own = own_nbrs(t, att))) {
            a->head = (a->head + 1) & (RING - 1);
            a->count--;
            continue;
        }
        terr = (b >> 5) & 3;
        if (own == 1 && a->count > 3 && rnd_n(terr + 1)) {
            /* exposed tile: try another one first to keep the front smooth */
            a->head = (a->head + 1) & (RING - 1);
            a->ring[(a->head + a->count - 1) & (RING - 1)] = t;
            continue;
        }
        key = (uint8_t)(terr | ((b & T_FALL) ? 4 : 0));
        if (def && dp->nPost && post_near(def, t)) key |= 8;
        if (have[key] && cacheOk) {
            cost = cCost[key];
            loss = cLoss[key];
            goto take;
        }
        mag = terrMag[terr];
        tc2 = terrCost2[terr];
        if (b & T_FALL) {
            mag *= 4;
            tc2 *= 4;
        }
        if (!def) {
            uint32_t w = (1000UL * tc2) / (a->troops + 1);
            if (w < 5) w = 5;
            if (w > 100) w = 100;
            cost = w * 2 * g_L16;
            loss = (uint32_t)(mag / (ap->type == PT_TRIBE ? 10 : 5)) * g_S;
        } else {
            uint32_t tmp;
            if (key & 8) {
                mag *= 5;
                tc2 *= 3;
            }
            if (dp->type == PT_TRIBE && ap->type != PT_TRIBE) mag = (mag * 179) >> 8;
            tmp = ((uint32_t)mag * lossRatio) >> 8;
            loss = (tmp * term) >> 8;
            cost = (sc8 * tc2) >> 4;
            cost = (cost * g_L16) >> 4;
            cost = (cost * 60) >> 8;
            cost = (cost * bAs) >> 8;
            cost = (cost * bD) >> 8;
            if (dp->traitor) {
                loss >>= 1;
                cost = (cost * 205) >> 8;
            }
            if (cost < 16) cost = 16;
        }
        cCost[key] = cost;
        cLoss[key] = loss;
        have[key] = 1;
    take:
        if (a->budget < cost) break;
        a->budget -= (uint24_t)cost;
        a->head = (a->head + 1) & (RING - 1);
        a->count--;
        taken++;
        if (def) sub_troops(def, dloss);
        PROF(1, conquer(t, att));
        if (!a->active) return; /* defender died: troops already refunded */
        for (i = 0; i < 4 && a->count < RING; i++) {
            tile_t n = (tile_t)(t + g_dirofs[i]);
            uint8_t nb = g_map[n];
            if (IS_LAND(nb) && (nb & T_OWNER) == def) {
                a->ring[(a->head + a->count) & (RING - 1)] = n;
                a->count++;
            }
        }
        if (loss >= a->troops) {
            a->active = 0;
            return;
        }
        a->troops -= loss;
        if (def) {
            if (!dp->alive) {
                end_attack(a, 0);
                return;
            }
            if (dp->tiles < 3) {
                annex_all(att, def);
                if (a->active) end_attack(a, 0);
                return;
            }
        }
    }
}

void attack_tick(void)
{
    attack_t *a;
    FOR_ATTACKS(a)
        if (a->active) run_attack(a);
}
