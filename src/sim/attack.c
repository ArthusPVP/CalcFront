/* land attacks: frontier sampling, per-tile cost and losses */
#include "sim.h"

attack_t g_at[MAX_ATTACKS];

#define BUDGET_MAX 60000

static const uint8_t terrMag[4] = {0, 80, 100, 120};
static const uint8_t terrCost2[4] = {0, 33, 40, 50}; /* tileCost * 2 */
static const uint8_t freeLoss[2][4] = {{0, 16, 20, 24}, {0, 8, 10, 12}}; /* terrMag / 5, / 10 */

/* land of player o? (owner first: inside a territory that test fails at once) */
#define LAND_OF(b, o) (((b) & T_OWNER) == (o) && IS_LAND(b))

/* land neighbours of the land tile at m owned by p, as 0, 1 or 2 (2 or more;
 * the callers need no more). The map frame is water, so the 4 neighbours
 * exist; W = map width. Written as jumps: a counting version compiles to
 * long branch-free select chains on the eZ80. */
FORCE_INLINE uint8_t own_nbrs(const uint8_t *m, uint8_t p, int W)
{
    if (LAND_OF(m[1], p)) goto n1;
    if (LAND_OF(m[-1], p)) goto n2;
    if (LAND_OF(m[W], p)) goto n3;
    return LAND_OF(m[-W], p) ? 1 : 0;
n1:
    if (LAND_OF(m[-1], p)) return 2;
n2:
    if (LAND_OF(m[W], p)) return 2;
n3:
    if (LAND_OF(m[-W], p)) return 2;
    return 1;
}

/* bits 8-31 of a 32-bit value, read as 3 bytes: a 32-bit shift is a library
 * call on the eZ80 */
FORCE_INLINE uint24_t shr8_32(const uint32_t *v)
{
#ifdef HOST
    return *v >> 8;
#else
    uint24_t r;
    memcpy(&r, (const uint8_t *)v + 1, 3);
    return r;
#endif
}
#define FITS24(v) (((const uint8_t *)&(v))[3] == 0) /* little endian */

/* b >> 5 (terrain | fallout << 2): clang turns every C spelling of it into
 * a shift loop call on the eZ80, three rotates do it */
FORCE_INLINE uint8_t top3(uint8_t b)
{
#if defined(HOST) || defined(CF_NOASM)
    return b >> 5;
#else
    uint8_t k;
    __asm__("rlca\n\trlca\n\trlca\n\tand\ta, 7" : "=a"(k) : "0"(b) : "cc");
    return k;
#endif
}

FORCE_INLINE uint8_t hi8(uint16_t v)
{
    u16b_t u;
    u.v = v;
    return u.b[1];
}

/* first tile from p to end (inclusive) that is land of player o, or 0. On
 * the calculator this is hand-written assembly (src/asm/scan.s); the C
 * version serves the PC harness and checks the assembly in profiling builds. */
const uint8_t *next_owned_c(const uint8_t *p, const uint8_t *end, uint8_t o)
{
    for (; p <= end; p++) {
        uint8_t b = *p;
        if ((b & T_OWNER) == o && (b & T_TERR)) return p;
    }
    return 0;
}
#if defined(HOST) || defined(CF_NOASM)
#define next_owned next_owned_c
#else
const uint8_t *next_owned(const uint8_t *p, const uint8_t *end, uint8_t o);
#endif

/* Sample the frontier (tiles of `def` touching `att`, att != def) into the
 * ring. With stopAtFirst it only answers "is there a border at all". */
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
        p = next_owned(p, end, def);
        while (p) {
            uint8_t nb;
            /* touches the attacker? (inline: this is the hottest loop) */
            nb = p[-1];
            if (LAND_OF(nb, att)) goto front;
        inrun:
            nb = p[1];
            if (LAND_OF(nb, att)) goto front;
            nb = p[W];
            if (LAND_OF(nb, att)) goto front;
            nb = p[-W];
            if (LAND_OF(nb, att)) goto front;
            goto next;
        front:
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
        next:
            /* the defender's land comes in runs: follow one here instead of
             * calling the scanner again for every tile. Inside a run the
             * tile on the left is the defender's, never the attacker's
             * (att != def for every caller), so its test is skipped. */
            if (p == end) break;
            p++;
            nb = *p;
            if (LAND_OF(nb, def)) goto inrun;
            p = next_owned(p + 1, end, def);
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
    attack_t *a = 0, *o, *same = 0;
    player_t *pl = &g_pl[att];
    bool countered = false;
    if (att == def || !pl->alive) return false;
    if (def && (!g_pl[def].alive || ALLIED(att, def))) return false;
    if (removeFromOwner && troops > pl->troops) troops = pl->troops;
    if (troops < 10) return false;

    FOR_ATTACKS(o) {
        if (!o->active) {
            if (!a) a = o;
        } else if (o->att == att && o->def == def && !o->retreat)
            same = o;
    }
    if (!same) {
        uint16_t found;
        if (!a) return false;
        a->att = att;
        a->def = def;
        a->border = 0;
        PROF(0, found = scan_front(a, att, def, false));
        if (!found) return false;
    }
    if (removeFromOwner) pl->troops -= troops;

    /* opposing attack: the two stacks cancel out, troop for troop (first,
     * as in OpenFront, also when this adds to an attack already running) */
    if (def)
        FOR_ATTACKS(o) {
            if (o->active && o->att == def && o->def == att && !o->retreat) {
                if (def == HUMAN) {
                    ui_event(EV_COUNTERED, att, 0);
                    countered = true;
                }
                if (o->troops > troops) {
                    o->troops -= troops;
                    return true;
                }
                troops -= o->troops;
                o->active = 0;
                if (att == HUMAN) ui_event(EV_STOPPED, def, 0);
                if (!troops) return true;
            }
        }
    if (same) {
        same->troops += troops;
        return true;
    }
    a->active = 1;
    a->retreat = 0;
    a->troops = troops;
    a->budget = 0;
    a->rescan = 24;
    if (def) {
        /* the defender's grudge, by difficulty; hitting back at a player
         * you already hate angers nobody (except on Easy), as in OpenFront */
        if (g_cfg.difficulty == DIFF_EASY || pl->rel[def] >= -50)
            rel_add(def, att, (int8_t)(-60 - 10 * (int8_t)g_cfg.difficulty));
        if (def == HUMAN && !countered) ui_event(EV_ATTACKED, att, 0);
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

/* Per-attack values that do not change within a tick, and the tile cost and
 * loss cache: they depend only on terrain, fallout and a nearby defense post
 * within a tick, so each combination (key) is computed once. On free land
 * the cost also depends on the attack's size, but only below 40000 troops.
 * Static rather than on the stack: run_attack's frame then stays small
 * enough for short IX offsets, which the eZ80 code uses everywhere. */
static struct {
    uint32_t sc8;
    uint16_t lossRatio, term, bAs, bD;
} K;
static uint16_t cCost[16]; /* capped at BUDGET_MAX, see tile_cost() */
static uint32_t cLoss[16];
static uint8_t have[16];

/* fill cache entry `key` (terrain | fallout << 2 | defense post << 3) */
static __attribute__((noinline)) void tile_cost(const attack_t *a, uint8_t key)
{
    const player_t *ap = &g_pl[a->att], *dp = &g_pl[a->def];
    uint8_t terr = key & 3;
    uint16_t mag = terrMag[terr];
    uint24_t tc2 = terrCost2[terr], cost;
    uint32_t loss;
    if (key & 4) {
        mag *= 4;
        tc2 *= 4;
    }
    if (!a->def) {
        /* w = 1000 * tc2 / (troops + 1) clamped to 5..100. 1000 * tc2 <=
         * 200000, so from 200000 troops on the quotient is 0; below, divide
         * (24 bits) only when the clamps do not settle it */
        uint24_t w = 5;
        if (a->troops < 200000) {
            uint24_t x = 1000u * tc2, d = (uint24_t)a->troops + 1;
            if (x >= 6 * d) {
                w = x / d;
                if (w > 100) w = 100;
            }
        }
        cost = w * 2 * g_L16;
        {
            /* mag / 5 (tribes: / 10): exact, terrMag is a multiple of 10 */
            uint8_t v = freeLoss[ap->type == PT_TRIBE][terr];
            if (key & 4) v *= 4;
            loss = (uint16_t)(v * g_S); /* <= 96 * 255 */
        }
    } else {
        uint32_t tmp, c;
        if (key & 8) {
            mag *= 5;
            tc2 *= 3;
        }
        if (dp->type == PT_TRIBE && ap->type != PT_TRIBE) mag = (mag * 179) >> 8;
        tmp = mulq8(mag, K.lossRatio);
        loss = mulq8(tmp, K.term);
        c = mulq4(K.sc8, (uint16_t)tc2);
        c = mulq4(c, g_L16);
        c = mulq8(c, 60);
        /* a Q8 factor of 256 (no large-territory malus) changes nothing */
        if (K.bAs != 256) c = mulq8(c, K.bAs);
        if (K.bD != 256) c = mulq8(c, K.bD);
        if (dp->traitor) {
            loss >>= 1;
            c = mulq8(c, 205);
        }
        if (c < 16) c = 16;
        /* the budget never exceeds BUDGET_MAX, and a full budget buys any
         * tile: a dearer tile acts exactly like one costing BUDGET_MAX */
        cost = c > BUDGET_MAX ? BUDGET_MAX : (uint24_t)c;
    }
    cCost[key] = (uint16_t)cost;
    cLoss[key] = loss;
    have[key] = 1;
}

static void run_attack(attack_t *a)
{
    uint8_t att = a->att, def = a->def, pops = 0, taken = 0;
    player_t *ap = &g_pl[att], *dp = &g_pl[def];
    uint24_t B;
    uint32_t dloss = 0;
    int W = (int)g_W;
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
    if (a->budget > BUDGET_MAX) a->budget = BUDGET_MAX;

    if (def) {
        uint32_t dt = dp->troops, q;
        uint24_t d = shr8_32(&a->troops) + 1; /* (troops >> 8) + 1 fits in 24 bits */
        uint16_t r8, a8, b8, bA, tiles = dp->tiles ? dp->tiles : 1;
        uint8_t f;
        /* 24-bit divisions whenever the defender's troops fit (as a rule) */
        if (FITS24(dt)) {
            q = (uint24_t)dt / d; /* ~ ratio * 256 */
            dloss = (uint24_t)dt / tiles;
        } else {
            q = dt / d;
            dloss = dt / tiles;
        }
        r8 = q > 60000 ? 60000 : (uint16_t)q;
        K.lossRatio = r8 < 154 ? 154 : (r8 > 512 ? 512 : r8);
        /* large-territory factors (256 = none, the usual case) */
        bA = K.bAs = K.bD = 256;
        if (ap->big8) {
            bA = 256 - hi8((uint16_t)(179 * ap->big8));
            K.bAs = 256 - hi8((uint16_t)(187 * ap->big8));
        }
        if (dp->big8) K.bD = 256 - hi8((uint16_t)(77 * dp->big8));
        /* (119 * bA >> 8) * bD >> 8; bA, bD <= 256: the products fit in 16
         * bits, and with both at 256 it is 119 */
        f = 119;
        if (bA != 256 || K.bD != 256) f = hi8((uint16_t)(hi8((uint16_t)(119u * bA)) * K.bD));
        K.term = (uint16_t)(f * g_S + (dloss > 20000 ? 20000 : (uint16_t)dloss));
        a8 = r8 < 210 ? 210 : (r8 > 1920 ? 1920 : r8);
        /* r8 / 20 clamped to 256..12800; r8 <= 60000 never reaches 12800 */
        b8 = r8 < 5120 ? 256 : r8 / 20;
        K.sc8 = b8 == 256 ? a8 : mulq8(a8, b8);
    }
    memset(have, 0, sizeof have);
    cacheOk = def || a->troops >= 40000;

    while (taken < 24 && pops < 48 && a->count) {
        tile_t t = a->ring[a->head];
        const uint8_t *m = g_map + t;
        uint8_t b = *m, own, key, h;
        uint24_t cost;
        uint32_t loss, tr;
        pops++;
        if (!LAND_OF(b, def) || !(own = own_nbrs(m, att, W))) {
            a->head = (uint8_t)(a->head + 1) & (RING - 1);
            a->count--;
            continue;
        }
        key = top3(b);
        if (own == 1 && a->count > 3 && rnd_n((key & 3) + 1)) {
            /* exposed tile: try another one first to keep the front smooth */
            h = (uint8_t)(a->head + 1) & (RING - 1);
            a->head = h;
            h = (uint8_t)(h + a->count - 1) & (RING - 1);
            a->ring[h] = t;
            continue;
        }
        if (def && dp->nPost && post_near(def, t)) key |= 8;
        if (!have[key] || !cacheOk) tile_cost(a, key);
        cost = cCost[key];
        loss = cLoss[key];
        if (a->budget < cost) {
            /* a full budget always buys the tile: OpenFront takes at least
             * one tile per tick, so even a hopeless push moves (and dies)
             * instead of holding its troops forever */
            if (a->budget < BUDGET_MAX) break;
            cost = a->budget;
        }
        a->budget -= cost;
        a->head = (uint8_t)(a->head + 1) & (RING - 1);
        a->count--;
        taken++;
        if (def) { /* sub_troops(def, dloss), inline */
            uint32_t dt = dp->troops;
            dp->troops = dt > dloss ? dt - dloss : 0;
        }
        PROF(1, conquer(t, att));
        if (!a->active) return; /* defender died: troops already refunded */
        {
            /* queue the defender's neighbours (+1, -1, +W, -W, as in
             * g_dirofs) while the ring has room */
            uint8_t c = a->count;
            h = a->head;
#define PUSH(ofs)                                                  \
    if (c < RING && LAND_OF(m[ofs], def)) {                        \
        uint8_t j = (uint8_t)(h + c) & (RING - 1);                 \
        a->ring[j] = (tile_t)(t + (ofs));                          \
        c++;                                                       \
    }
            PUSH(1)
            PUSH(-1)
            PUSH(W)
            PUSH(-W)
#undef PUSH
            a->count = c;
        }
        tr = a->troops;
        if (loss >= tr) {
            a->active = 0;
            return;
        }
        a->troops = tr - loss;
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
