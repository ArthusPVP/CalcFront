/* land attacks: frontier sampling, per-tile cost and losses */
#include "sim.h"

attack_t g_at[MAX_ATTACKS];

#define BUDGET_MAX 60000

static const uint8_t terrMag[4] = {0, 80, 100, 120};
static const uint8_t terrCost2[4] = {0, 33, 40, 50}; /* tileCost * 2 */
static const uint8_t freeLoss[2][4] = {{0, 16, 20, 24}, {0, 8, 10, 12}}; /* terrMag / 5, / 10 */

/* land of player o? (owner first: inside a territory that test fails at once) */
#define LAND_OF(b, o) (((b) & T_OWNER) == (o) && IS_LAND(b))

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

/* the low 24 bits of a 32-bit value */
FORCE_INLINE uint24_t lo24(const uint32_t *v)
{
#ifdef HOST
    return *v & 0xFFFFFF;
#else
    uint24_t r;
    memcpy(&r, v, 3);
    return r;
#endif
}

/* &g_pl[p]: the index shift is a library call on the eZ80 */
FORCE_INLINE player_t *pl_of(uint8_t p)
{
#ifdef HOST
    return &g_pl[p];
#else
    union {
        uint24_t v;
        uint8_t b[3];
    } u;
    u.v = 0;
    u.b[1] = p;
    return (player_t *)((uint8_t *)g_pl + u.v);
#endif
}

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

/* Arithmetic helpers, hand-written in src/asm/attack.s on the calculator
 * (library-call free); these C versions serve the PC harness and check the
 * assembly in profiling builds. */
/* rnd_n(n) for n <= 255 */
uint8_t rnd_below_c(uint8_t n) { return (uint8_t)rnd_n(n); }
/* (x * k) >> 8; every caller keeps x * k below 2^32 (the result fits in 24
 * bits) */
uint24_t mul_q8_c(uint24_t x, uint16_t k) { return (uint24_t)(((uint32_t)x * k) >> 8); }
/* cost of a tile of free land: w = x / d clamped to 5..100 (5 below 6),
 * times 2 * g_L16; x <= 200000, 1 <= d <= 200001 */
uint16_t free_cost_c(uint24_t x, uint24_t d)
{
    uint24_t w = 5;
    if (x >= 6 * d) {
        w = x / d;
        if (w > 100) w = 100;
    }
    return (uint16_t)(w * 2 * g_L16);
}
/* x / d (d >= 1): the assembly loops once per quotient bit, not 24 times */
uint24_t udiv24_c(uint24_t x, uint24_t d) { return x / d; }
#if defined(HOST) || defined(CF_NOASM)
#define rnd_below rnd_below_c
#define mul_q8 mul_q8_c
#define free_cost free_cost_c
#define udiv24 udiv24_c
#else
uint8_t rnd_below(uint8_t n);
uint24_t mul_q8(uint24_t x, uint16_t k);
uint16_t free_cost(uint24_t x, uint24_t d);
uint24_t udiv24(uint24_t x, uint24_t d);
#endif

/* Frontier tiles (land of def with a land neighbour of att) in `rows` rows
 * of `len` tiles from p, in row order: the first RING go to ring[], later
 * ones replace a random entry (reservoir sampling). Returns their count;
 * ring = 0 only answers "is there one" (returns 0 or 1). On the calculator
 * this is hand-written assembly (src/asm/front.s); this C version serves the
 * PC harness and checks the assembly in profiling builds. */
uint16_t front_scan_c(const uint8_t *p, uint24_t len, uint24_t rows, uint8_t def, uint8_t att,
                      tile_t *ring)
{
    uint16_t n = 0;
    int W = (int)g_W;
    for (; rows; rows--, p += W) {
        const uint8_t *q = p, *end = p + len - 1;
        for (;; q++) {
            uint8_t nb = *q;
            if (!LAND_OF(nb, def)) goto skip;
            /* the first tile of a run of def's land tests its left
             * neighbour, the next ones skip it (it is def's) */
            nb = q[-1];
            if (LAND_OF(nb, att)) goto front;
        inrun:
            nb = q[1];
            if (LAND_OF(nb, att)) goto front;
            nb = q[W];
            if (LAND_OF(nb, att)) goto front;
            nb = q[-W];
            if (LAND_OF(nb, att)) goto front;
            goto next;
        front:
            if (!ring) return 1;
            {
                tile_t t = (tile_t)(q - g_map);
                if (n < RING)
                    ring[n] = t;
                else {
                    uint16_t j = rnd_n(n + 1);
                    if (j < RING) ring[j] = t;
                }
            }
            n++;
        next:
            if (q == end) break;
            q++;
            nb = *q;
            if (LAND_OF(nb, def)) goto inrun;
        skip:
            if (q == end) break;
        }
    }
    return n;
}
#if defined(HOST) || defined(CF_NOASM)
#define front_scan front_scan_c
#else
uint16_t front_scan(const uint8_t *p, uint24_t len, uint24_t rows, uint8_t def, uint8_t att,
                    tile_t *ring);
#endif

/* Sample the frontier (tiles of `def` touching `att`, att != def) into the
 * ring. With stopAtFirst it only answers "is there a border at all". */
static uint16_t scan_front(attack_t *a, uint8_t att, uint8_t def, bool stopAtFirst)
{
    player_t *pl = &g_pl[att];
    uint16_t x, x0, x1, y0, y1;
    uint16_t n;
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
    n = front_scan(g_map + g_rowStart[y0] + x0, (uint24_t)(x1 - x0 + 1), (uint24_t)(y1 - y0 + 1), def,
                   att, stopAtFirst ? 0 : a->ring);
    if (a) {
        a->head = 0;
        a->count = n > RING ? RING : (uint8_t)n;
        a->border = n;
        /* shuffle so the front advances evenly */
        for (x = a->count; x > 1; x--) {
            uint8_t j = rnd_below((uint8_t)x);
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

static bool attack_open(attack_t *a, attack_t *same, uint8_t att, uint8_t def, uint32_t troops,
                        uint8_t kind);

bool attack_start(uint8_t att, uint8_t def, uint32_t troops, bool removeFromOwner)
{
    attack_t *a = 0, *o, *same = 0;
    player_t *pl = &g_pl[att];
    if (att == def || !pl->alive) return false;
    if (def && (!g_pl[def].alive || ALLIED(att, def))) return false;
    if (removeFromOwner && troops > pl->troops) troops = pl->troops;
    if (troops < 10) return false;

    FOR_ATTACKS(o) {
        if (!o->active) {
            if (!a) a = o;
        } else if (o->active == 1 && o->att == att && o->def == def && !o->retreat)
            same = o; /* boat attacks (active 2) are never merged into */
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
    return attack_open(a, same, att, def, troops, 1);
}

/* The common end of attack_start and attack_start_at: cancel against the
 * defender's attack on us, then add to `same` or open `a` as `kind`. */
static bool attack_open(attack_t *a, attack_t *same, uint8_t att, uint8_t def, uint32_t troops,
                        uint8_t kind)
{
    attack_t *o;
    player_t *pl = &g_pl[att];
    bool countered = false;
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
    a->active = kind;
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

/* A landed boat's attack (OpenFront): it starts from the land around the
 * landing tile (already taken) and only grows from the tiles it takes, never
 * along the rest of the border, and is never merged with another attack.
 * active = 2 marks it; it ends, refunding its troops, when its front is gone.
 * The troops come from the boat, not from the attacker's army. */
bool attack_start_at(uint8_t att, uint8_t def, uint32_t troops, tile_t land)
{
    attack_t *a = 0, *o;
    uint8_t i;
    if (att == def || !g_pl[att].alive) return false;
    if (def && (!g_pl[def].alive || ALLIED(att, def))) return false;
    if (troops < 10) return false;
    FOR_ATTACKS(o)
        if (!o->active) {
            a = o;
            break;
        }
    if (!a) return false;
    a->att = att;
    a->def = def;
    a->border = 0; /* the budget follows the size of the local front */
    a->head = a->count = 0;
    for (i = 0; i < 4; i++) {
        tile_t n = (tile_t)(land + g_dirofs[i]);
        uint8_t b = g_map[n];
        if (IS_LAND(b) && (b & T_OWNER) == def) a->ring[a->count++] = n;
    }
    if (!a->count) return false;
    return attack_open(a, 0, att, def, troops, 2);
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
 * enough for short IX offsets, which the eZ80 code uses everywhere.
 * Every value fits in 24 bits (the largest loss is 2400 * 512 / 256 *
 * 50345 / 256 < 950000), so no 32-bit library call is needed. */
typedef struct {
    uint16_t lossRatio, term, bAs, bD;
    uint24_t sc8;
    uint8_t traitor, L16;      /* defender is a traitor; g_L16 */
    uint8_t tribeMag;          /* mag * 0.7 (a tribe attacked by a nation) */
} atk_K_t;
__attribute__((used)) atk_K_t atk_K;
#define K atk_K
/* (global: src/asm/attack.s reads them) */
__attribute__((used)) uint16_t atk_cCost[16]; /* capped at BUDGET_MAX, see tile_cost() */
__attribute__((used)) uint32_t atk_cLoss[16]; /* < 2^24: 4 bytes for a shift-indexed array */
__attribute__((used)) uint8_t atk_have[16];

/* What the tile loop of an attack works with (the assembly reads it: the
 * layout is checked below) */
typedef struct {
    attack_t *a;
    player_t *dp;                      /* defender */
    void (*conquer)(tile_t t, uint8_t p);
    uint24_t loss, t;                  /* scratch: the tile being taken */
    uint32_t dloss;                    /* the defender's troops per tile */
    uint8_t def, att, cacheOk, tribe8; /* tribe8: 8 when att is a tribe */
    uint8_t taken, pops, key;
} atk_ctx_t;
__attribute__((used)) atk_ctx_t atk_ctx;

/* The cost of a tile of a player (not its loss) depends only on the key and
 * on sc8, bAs, bD, the traitor flag and g_L16, which change rarely (sc8
 * stays at 210 while the attacker is much larger): kept across ticks and
 * attacks for as long as they do not change. */
#define TAGLEN (offsetof(atk_K_t, tribeMag) - offsetof(atk_K_t, bAs)) /* bAs..L16 */
__attribute__((used)) uint8_t atk_costTag[TAGLEN], atk_tagHave[16];
static uint16_t tagCost[16];

/* free land: 1000 * tileCost * 2 by terrain | fallout << 2, and the losses
 * by tribe << 3 | terrain | fallout << 2 for the current area scale */
__attribute__((used)) const uint32_t atk_freeX[8] = {0, 33000, 40000, 50000, 0, 132000, 160000, 200000};
__attribute__((used)) uint16_t atk_freeLossT[16], atk_cost5;
__attribute__((used)) uint8_t atk_freeS, atk_freeL16;
__attribute__((used)) uint24_t atk_r8, atk_dl; /* scratch for the assembly */

/* cost of a tile of free land for an attack of `troops` */
static uint16_t free_cost_of(const attack_t *a, uint8_t key)
{
    /* w = 1000 * tc2 / (troops + 1) clamped to 5..100; 1000 * tc2 <=
     * 200000, so from 33333 troops on the quotient is below 6: w = 5 */
    if (FITS24(a->troops) && (uint24_t)a->troops < 33333)
        return free_cost_c(lo24(&atk_freeX[key & 7]), (uint24_t)a->troops + 1);
    return atk_cost5;
}

/* fill cache entry `key` (terrain | fallout << 2 | defense post << 3) of an
 * attack on a player */
__attribute__((noinline, used)) void atk_tile_cost(uint8_t key)
{
    uint8_t terr = key & 3;
    uint16_t mag = terrMag[terr];
    uint24_t loss;
    if (key & 4) mag *= 4;
    if (key & 8) mag *= 5;
    if (K.tribeMag) mag = (uint16_t)mul_q8(mag, 179);
    loss = mul_q8(mul_q8(mag, K.lossRatio), K.term);
    if (K.traitor) loss >>= 1;
    atk_cLoss[key] = loss;
    atk_have[key] = 1;
    if (!atk_tagHave[key]) {
        /* the products stay under 2^32 (tc2 <= 600, sc8 <= 22500, L16 <=
         * 255): mulq4(x, k) = mul_q8(x, 16 k) */
        uint24_t c;
        uint16_t tc16 = (uint16_t)(terrCost2[terr] * 16);
        if (key & 4) tc16 *= 4;
        if (key & 8) tc16 *= 3;
        c = mul_q8(K.sc8, tc16);
        c = mul_q8(c, (uint16_t)(g_L16 * 16));
        c = mul_q8(c, 60);
        /* a Q8 factor of 256 (no large-territory malus) changes nothing */
        if (K.bAs != 256) c = mul_q8(c, K.bAs);
        if (K.bD != 256) c = mul_q8(c, K.bD);
        if (K.traitor) c = mul_q8(c, 205);
        if (c < 16) c = 16;
        /* the budget never exceeds BUDGET_MAX, and a full budget buys any
         * tile: a dearer tile acts exactly like one costing BUDGET_MAX */
        tagCost[key] = c > BUDGET_MAX ? BUDGET_MAX : (uint16_t)c;
        atk_tagHave[key] = 1;
    }
    atk_cCost[key] = tagCost[key];
}

/* Pops tiles of the ring of atk_ctx.a (while atk_ctx.pops < 48, counting
 * them there) until one is worth attacking: land of the defender touching
 * the attacker, not put back as exposed (a tile with one attacker neighbour
 * goes back to the end of the ring, if more than 3 are queued, when
 * rnd_n(terrain + 1) is not 0, to keep the front smooth). Dead tiles leave
 * the ring. Returns its key (terrain | fallout << 2), the tile left at
 * ring[head], or 0xFF for none. Assembly on the calculator. */
uint8_t pop_tile_c(void)
{
    attack_t *a = atk_ctx.a;
    while (atk_ctx.pops < 48 && a->count) {
        tile_t t = a->ring[a->head];
        uint8_t b = g_map[t], own = 0, key, h, i;
        atk_ctx.pops++;
        if (LAND_OF(b, a->def))
            for (i = 0; i < 4; i++)
                if (LAND_OF(g_map[(tile_t)(t + g_dirofs[i])], a->att)) own++;
        if (!own) {
            a->head = (uint8_t)(a->head + 1) & (RING - 1);
            a->count--;
            continue;
        }
        key = top3(b);
        if (own == 1 && a->count > 3 && rnd_below_c((uint8_t)((key & 3) + 1))) {
            h = (uint8_t)(a->head + 1) & (RING - 1);
            a->head = h;
            h = (uint8_t)(h + a->count - 1) & (RING - 1);
            a->ring[h] = t;
            continue;
        }
        return key;
    }
    return 0xFF;
}

/* queue the defender's land around the conquered tile t (+1, -1, +W, -W,
 * as in g_dirofs) at the end of the ring while it has room */
void push_nbrs_c(attack_t *a, tile_t t)
{
    uint8_t i, c = a->count;
    for (i = 0; i < 4; i++) {
        tile_t n = (tile_t)(t + g_dirofs[i]);
        if (c < RING && LAND_OF(g_map[n], a->def)) a->ring[(uint8_t)(a->head + c++) & (RING - 1)] = n;
    }
    a->count = c;
}
#if !defined(HOST) && !defined(CF_NOASM)
/* the assembly's offsets */
_Static_assert(offsetof(attack_t, att) == 1 && offsetof(attack_t, def) == 2, "attack_t");
_Static_assert(offsetof(attack_t, head) == 14 && offsetof(attack_t, count) == 15, "attack_t");
_Static_assert(offsetof(attack_t, ring) == 18, "attack_t");
#endif


/* The tile loop of run_attack (atk_ctx set up): takes up to 24 tiles in up
 * to 48 pops while the budget lasts. Returns 0 when done, 1 when the attack
 * is over (conquered out, or out of troops), 2 when the defender died (the
 * caller ends the attack), 3 when the defender is down to 2 tiles (the
 * caller annexes them and ends it). Assembly on the calculator. */
uint8_t atk_loop_c(void)
{
    atk_ctx_t *x = &atk_ctx;
    attack_t *a = x->a;
    player_t *dp = x->dp;
    uint8_t def = x->def;
    x->taken = x->pops = 0;
    while (x->taken < 24) {
        tile_t t;
        uint8_t key = pop_tile_c();
        uint24_t cost, loss;
        if (key == 0xFF) break;
        t = a->ring[a->head];
        if (def && dp->nPost && post_near(def, t)) key |= 8;
        if (def) {
            if (!atk_have[key]) atk_tile_cost(key);
            cost = atk_cCost[key];
            loss = lo24(&atk_cLoss[key]);
        } else {
            /* free land: the cost depends on the troops (kept for the tick
             * from 40000 troops on, as the losses always are) */
            if (!atk_have[key] || !x->cacheOk) {
                atk_have[key] = 1;
                atk_cCost[key] = free_cost_of(a, key);
            }
            cost = atk_cCost[key];
            loss = atk_freeLossT[x->tribe8 | key];
        }
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
        x->taken++;
        if (def) { /* sub_troops(def, dloss), inline */
            uint32_t dt = dp->troops;
            dp->troops = dt > x->dloss ? dt - x->dloss : 0;
        }
        x->conquer(t, x->att);
        if (!a->active) return 1; /* defender died: troops already refunded */
        push_nbrs_c(a, t);
        /* troops -= loss, or the attack dies (24-bit when they fit) */
        if (FITS24(a->troops)) {
            uint24_t tr = (uint24_t)a->troops;
            if (loss >= tr) {
                a->active = 0;
                return 1;
            }
            a->troops = tr - loss;
        } else
            a->troops -= loss;
        if (def) {
            if (!dp->alive) return 2;
            if (dp->tiles < 3) return 3;
        }
    }
    return 0;
}
#if defined(HOST) || defined(CF_NOASM)
#define atk_loop atk_loop_c
#else
uint8_t atk_loop(void);
_Static_assert(offsetof(atk_ctx_t, dp) == 3 && offsetof(atk_ctx_t, conquer) == 6, "atk_ctx_t");
_Static_assert(offsetof(atk_ctx_t, loss) == 9 && offsetof(atk_ctx_t, t) == 12, "atk_ctx_t");
_Static_assert(offsetof(atk_ctx_t, dloss) == 15 && offsetof(atk_ctx_t, def) == 19, "atk_ctx_t");
_Static_assert(offsetof(atk_ctx_t, taken) == 23 && offsetof(atk_ctx_t, key) == 25, "atk_ctx_t");
_Static_assert(offsetof(attack_t, troops) == 4 && offsetof(attack_t, budget) == 8, "attack_t");
_Static_assert(offsetof(player_t, alive) == 1 && offsetof(player_t, troops) == 3, "player_t");
_Static_assert(offsetof(player_t, tiles) == 20 && offsetof(player_t, nPost) == 53, "player_t");
#endif

/* the troop ratio (r8, returned) and the defender's losses per tile when
 * its troops do not fit in 24 bits (atk_ctx.dloss, and atk_dl for the term,
 * capped where it no longer matters) */
__attribute__((used)) uint24_t atk_big_div(uint24_t d)
{
    const player_t *dp = atk_ctx.dp;
    uint32_t dt = dp->troops, q = dt / d;
    atk_ctx.dloss = dt / (dp->tiles ? dp->tiles : 1);
    atk_dl = FITS24(atk_ctx.dloss) ? (uint24_t)atk_ctx.dloss : 20000;
    return q > 60000 ? 60000 : (uint24_t)q;
}

/* free land losses: mag / 5 (tribes: / 10), exact as terrMag is a multiple
 * of 10, times 4 on fallout, times the area scale; and the cost at w = 5 */
__attribute__((used)) void atk_free_tables(void)
{
    uint8_t k;
    atk_freeS = g_S;
    atk_freeL16 = g_L16;
    atk_cost5 = (uint16_t)(10 * g_L16);
    for (k = 0; k < 16; k++) {
        uint8_t v = freeLoss[k >> 3][k & 3];
        if (k & 4) v *= 4;
        atk_freeLossT[k] = (uint16_t)(v * g_S); /* <= 96 * 255 */
    }
}

/* conquer, timed in the profiling build */
__attribute__((used)) void atk_conquer(tile_t t, uint8_t p) { PROF(1, conquer(t, p)); }

/* The tick's budget (border size + rnd_n(5), times 64), the per-tick values
 * of the tile cost (atk_K) and atk_ctx for the tile loop. Assembly on the
 * calculator (src/asm/attack.s). */
void atk_setup_c(attack_t *a)
{
    uint8_t att = a->att, def = a->def;
    player_t *ap = pl_of(att), *dp = pl_of(def);
    uint24_t B;

    atk_ctx.a = a;
    atk_ctx.dp = dp;
    atk_ctx.conquer = atk_conquer;
    atk_ctx.def = def;
    atk_ctx.att = att;
    atk_ctx.tribe8 = ap->type == PT_TRIBE ? 8 : 0;
    B = a->border;
    if (B < a->count) B = a->count;
    if (B > 600) B = 600;
    B += rnd_below_c(5);
    B = a->budget + B * 64;
    a->budget = B > BUDGET_MAX ? BUDGET_MAX : B;

    if (def) {
        uint32_t dt = dp->troops;
        uint24_t d = shr8_32(&a->troops) + 1; /* (troops >> 8) + 1 fits in 24 bits */
        uint24_t r8, dl;
        uint16_t bA;
        uint8_t f;
        if (FITS24(dt)) {
            r8 = udiv24_c((uint24_t)dt, d); /* ~ ratio * 256 */
            atk_ctx.dloss = dl = udiv24_c((uint24_t)dt, dp->tiles ? dp->tiles : 1);
        } else {
            r8 = atk_big_div(d);
            dl = atk_dl;
        }
        if (r8 > 60000) r8 = 60000;
        K.lossRatio = (uint16_t)(r8 < 154 ? 154 : (r8 > 512 ? 512 : r8));
        /* large-territory factors (256 = none, the usual case) */
        bA = 256 - hi8((uint16_t)(179 * ap->big8));
        K.bAs = 256 - hi8((uint16_t)(187 * ap->big8));
        K.bD = 256 - hi8((uint16_t)(77 * dp->big8));
        /* (119 * bA >> 8) * bD >> 8; bA, bD <= 256: the products fit in 16
         * bits */
        f = hi8((uint16_t)(hi8((uint16_t)(119u * bA)) * K.bD));
        K.term = (uint16_t)(f * g_S + (dl > 20000 ? 20000 : (uint16_t)dl));
        /* a8 = r8 clamped to 210..1920, times r8 / 20 in Q8 (clamped to
         * 256..12800: r8 <= 60000 never reaches 12800) */
        K.sc8 = r8 < 5120 ? (r8 < 210 ? 210 : (r8 > 1920 ? 1920 : r8)) : mul_q8_c(1920, (uint16_t)udiv24_c(r8, 20));
        K.traitor = dp->traitor != 0;
        K.L16 = g_L16;
        K.tribeMag = dp->type == PT_TRIBE && ap->type != PT_TRIBE;
        if (memcmp(&K.bAs, atk_costTag, TAGLEN)) {
            memcpy(atk_costTag, &K.bAs, TAGLEN);
            memset(atk_tagHave, 0, sizeof atk_tagHave);
        }
    } else if (g_S != atk_freeS || g_L16 != atk_freeL16)
        atk_free_tables();
    memset(atk_have, 0, sizeof atk_have);
    atk_ctx.cacheOk = def || !FITS24(a->troops) || (uint24_t)a->troops >= 40000;
}
#if defined(HOST) || defined(CF_NOASM)
#define atk_setup atk_setup_c
#else
void atk_setup(attack_t *a);
_Static_assert(offsetof(atk_K_t, term) == 2 && offsetof(atk_K_t, bAs) == 4, "atk_K_t");
_Static_assert(offsetof(atk_K_t, bD) == 6 && offsetof(atk_K_t, sc8) == 8, "atk_K_t");
_Static_assert(offsetof(atk_K_t, traitor) == 11 && offsetof(atk_K_t, tribeMag) == 13, "atk_K_t");
_Static_assert(offsetof(attack_t, border) == 12 && offsetof(player_t, big8) == 47, "offsets");
_Static_assert(offsetof(player_t, traitor) == 48 && PT_TRIBE == 3, "offsets");
#endif

static void run_attack(attack_t *a)
{
    uint8_t att = a->att, def = a->def;
    player_t *ap = pl_of(att), *dp = pl_of(def);

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
    if (a->active == 2) {
        if (!a->count) { /* the front around the landing is gone */
            end_attack(a, 0);
            return;
        }
    } else if (a->count == 0 || --a->rescan == 0) {
        uint16_t found;
        a->rescan = 24;
        PROF(0, found = scan_front(a, att, def, false));
        if (!found) {
            end_attack(a, 0);
            return;
        }
    }
    atk_setup(a);
    switch (atk_loop()) {
    case 2:
        end_attack(a, 0);
        break;
    case 3:
        annex_all(att, def);
        if (a->active) end_attack(a, 0);
        break;
    }
}

void attack_tick(void)
{
    attack_t *a;
    FOR_ATTACKS(a)
        if (a->active) run_attack(a);
}

#ifdef CF_PERF
/* Profiling build: every assembly routine of src/asm/attack.s and front.s
 * against its C version, on random and edge inputs, and on the live map
 * (fronts, and the tile loop on copies of the running attacks, conquer()
 * replaced by a hook that records its calls), from the same generator state
 * when they draw. Returns the number of mismatches. */
static uint8_t stS[4];
static uint32_t hookH;
static void hook(tile_t t, uint8_t p) { hookH = hookH * 33 + t + p; }
/* the generator state, and back to stS */
static uint32_t st_back(void)
{
    uint32_t v;
    rnd_get((uint8_t *)&v);
    rnd_set(stS);
    return v;
}

uint16_t attack_selftest(void)
{
    /* scratch: the route search heap (free outside a search) */
    attack_t *t1 = (attack_t *)g_mem->hq, *t2 = t1 + 1;
    uint8_t st[4];
    uint16_t bad = 0, k;
    rnd_get(st);
    for (k = 0; k < 3000; k++) {
        uint24_t x = (uint24_t)rnd() * 251 + rnd(), d = (x >> (k & 15)) + (k >> 4 & 1), y = x;
        uint16_t m = rnd();
        uint32_t s;
        uint8_t v;
        if (!d) d = 1;
        bad += udiv24(x, d) != udiv24_c(x, d);
        if (k & 1) /* products below 2^32 */
            y &= 0xFFFF;
        else
            m &= 0xFF;
        bad += mul_q8(y, m) != mul_q8_c(y, m);
        x %= 200001;
        d = d % 200001 + 1;
        bad += free_cost(x, d) != free_cost_c(x, d);
        rnd_get(stS);
        v = rnd_below((uint8_t)k);
        s = st_back();
        bad += v != rnd_below_c((uint8_t)k) || s != st_back();
    }
    for (k = 0; k < 400; k++) {
        uint16_t x0 = (uint16_t)(rnd_n(g_W - 2) + 1), y0 = (uint16_t)(rnd_n(g_H - 2) + 1), x1, y1, n;
        uint8_t att = (uint8_t)rnd_n(g_nplayers + 1), def = (uint8_t)rnd_n(g_nplayers + 1);
        const uint8_t *p;
        uint32_t s;
        x1 = (uint16_t)(x0 + rnd_n(g_W - 1 - x0));
        y1 = (uint16_t)(y0 + rnd_n(g_H - 1 - y0));
        if (k & 1) x0 = 1, x1 = (uint16_t)(g_W - 2); /* rows of 256+ tiles */
        if ((k & 7) == 2) att = def;                 /* long fronts: sampling */
        p = g_map + g_rowStart[y0] + x0;
        memset(t1, 0, 2 * sizeof *t1);
        rnd_get(stS);
        n = front_scan(p, (uint24_t)(x1 - x0 + 1), (uint24_t)(y1 - y0 + 1), def, att, (k & 7) == 6 ? 0 : t1->ring);
        s = st_back();
        bad += n != front_scan_c(p, (uint24_t)(x1 - x0 + 1), (uint24_t)(y1 - y0 + 1), def, att,
                                 (k & 7) == 6 ? 0 : t2->ring) ||
               s != st_back() || memcmp(t1, t2, sizeof *t1);
    }
    for (k = 0; k < 4 * MAX_ATTACKS; k++) {
        const attack_t *a = &g_at[k % MAX_ATTACKS];
        player_t *dp = pl_of(a->def);
        uint32_t dt = dp->troops, s, h;
        uint8_t r;
        if (!a->active) continue;
        memcpy(t1, a, sizeof *t1);
        if (k >= MAX_ATTACKS) { /* buy (and die) more, other rings */
            t1->budget = 60000;
            t1->troops = 1 + rnd_n(k & 1 ? 60000 : 3000);
            t1->head = (uint8_t)rnd() & (RING - 1);
            t1->count = (uint8_t)(RING - rnd_n(k & 2 ? 4 : RING));
        }
        memcpy(t2, t1, sizeof *t1);
        rnd_get(stS);
        atk_setup(t1);
        memcpy(t1 + 2, &atk_ctx, sizeof atk_ctx);
        ((atk_ctx_t *)(t1 + 2))->a = t2; /* the C version below is given the copy */
        memcpy((uint8_t *)(t1 + 2) + sizeof atk_ctx, &K, sizeof K);
        atk_ctx.conquer = hook;
        hookH = 0;
        r = atk_loop();
        h = hookH ^ dp->troops;
        s = st_back();
        dp->troops = dt;
        atk_setup_c(t2);
        bad += memcmp(t1 + 2, &atk_ctx, sizeof atk_ctx) || memcmp((uint8_t *)(t1 + 2) + sizeof atk_ctx, &K, sizeof K);
        atk_ctx.conquer = hook;
        hookH = 0;
        bad += r != atk_loop_c() || h != (hookH ^ dp->troops) || s != st_back() || memcmp(t1, t2, sizeof *t1);
        dp->troops = dt;
    }
    rnd_set(st);
    return bad;
}
#endif
