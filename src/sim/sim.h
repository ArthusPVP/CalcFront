/* CalcFront - portable simulation core (no calculator headers in here) */
#ifndef SIM_H
#define SIM_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stddef.h>

#ifdef HOST
typedef uint32_t uint24_t;
#endif

/* ---------------------------------------------------------------- map --- */
/* One byte per tile, row stride = map width (g_W, at least 128).
 *   bits 5-6 terrain (0 water, 1 plains, 2 highland, 3 mountain)
 *   land : bits 0-4 owner (0 = nobody), bit 7 fallout
 *   water: bits 0-2 water body id (0 = lake or map frame: not navigable),
 *          bits 3-4 and 7 scratch for pathfinding
 * The outer frame is water, so the 4 neighbours of a land tile always exist. */
typedef uint16_t tile_t;

#define T_OWNER 0x1F
#define T_TERR 0x60
#define T_FALL 0x80
#define T_COMP 0x07
#define NO_TILE 0xFFFF
#define MAP_MAX_H 512

extern uint8_t *g_map;
extern uint8_t *g_stbits;          /* 1 bit per tile: a structure stands here */
extern uint16_t g_W, g_H;
extern uint16_t g_rowStart[MAP_MAX_H + 1];
extern uint16_t g_rowOfHi[256];    /* row holding tile (hi << 8) */
extern int16_t g_dirofs[4];        /* +1, -1, +W, -W */

/* big scratch buffers, provided by the platform (on the calculator: a
 * temporary RAM variable, since static RAM is full) */
#define HEAPN 2048
#define VMAX 2048
typedef struct {
    uint8_t railbits[8192];   /* 1 bit per tile: railway */
    tile_t hq[HEAPN];         /* route search: heap of tiles */
    uint16_t hk[HEAPN];       /*               heap keys */
    tile_t vlist[VMAX];       /*               visited tiles */
} sim_mem_t;
extern sim_mem_t *g_mem;
#define g_railbits (g_mem->railbits)

/* The eZ80 has no barrel shifter: multi-bit shifts are slow library loops.
 * These helpers only use byte access and lookup tables. */
typedef union {
    uint16_t v;
    uint8_t b[2];
} u16b_t;

/* byte tables for shift-free bitmap access: bit t of a bitmap is bit
 * g_bitMask[lo] of byte g_bitHi[hi] + g_bitLo[lo] (hi, lo: bytes of t).
 * Filled by sim_tables() at startup (static RAM costs no program RAM). */
extern uint16_t g_bitHi[256];
extern uint8_t g_bitLo[256], g_bitMask[256];
void sim_tables(void);

#define FORCE_INLINE static inline __attribute__((always_inline))

/* row of tile t: ty_in() is inlined for the hot paths (distances, route
 * search, conquests, drawing), ty() is a call everywhere else */
FORCE_INLINE uint16_t ty_in(tile_t t)
{
    u16b_t u;
    uint16_t y;
    u.v = t;
    y = g_rowOfHi[u.b[1]];
    while (t >= g_rowStart[y + 1]) y++;
    return y;
}
uint16_t ty(tile_t t);
uint16_t tx(tile_t t);
FORCE_INLINE uint8_t bitget(const uint8_t *a, tile_t t)
{
    u16b_t u;
    u.v = t;
    return a[g_bitHi[u.b[1]] + g_bitLo[u.b[0]]] & g_bitMask[u.b[0]];
}
FORCE_INLINE void bitset(uint8_t *a, tile_t t)
{
    u16b_t u;
    u.v = t;
    a[g_bitHi[u.b[1]] + g_bitLo[u.b[0]]] |= g_bitMask[u.b[0]];
}
FORCE_INLINE void bitclr(uint8_t *a, tile_t t)
{
    u16b_t u;
    u.v = t;
    a[g_bitHi[u.b[1]] + g_bitLo[u.b[0]]] &= (uint8_t)~g_bitMask[u.b[0]];
}

#define TILE(x, y) ((tile_t)(g_rowStart[y] + (x)))
#define IS_LAND(b) ((b) & T_TERR)
#define OWNER_OF(t) (g_map[t] & T_OWNER) /* only valid on land tiles */
#define BITGET(a, t) (bitget((a), (t)) != 0)

#define MAP_NAME_LEN 16
#define NATION_NAME_LEN 14
#define MAX_MAP_NATIONS 40

typedef struct {
    uint16_t x, y;
    char name[NATION_NAME_LEN];
} map_nation_t;

typedef struct {
    char name[MAP_NAME_LEN];
    uint16_t w, h;
    uint16_t land;
    uint16_t scale16;
    uint16_t off;
    uint8_t comp;
    uint8_t nvar, var;
    uint8_t nnations;
} map_info_t;

extern map_info_t g_mi;
extern map_nation_t g_nat[MAX_MAP_NATIONS];

/* --------------------------------------------------------------- game --- */
#define MAXP 32 /* ids 1..31, 0 = nobody */
#define HUMAN 1

enum { PT_NONE, PT_HUMAN, PT_NATION, PT_TRIBE };
enum { DIFF_EASY, DIFF_MEDIUM, DIFF_HARD, DIFF_IMPOSSIBLE };

/* Padded to 256 bytes (and units to 32): indexing g_pl[p] is then a shift
 * instead of a call to the multiply routine. Saves store PLAYER_BYTES. */
typedef union {
    struct {
        uint8_t type;
        uint8_t alive;
        uint8_t ratio;      /* attack ratio in % */
        uint32_t troops;
        uint32_t maxTroops;
        uint32_t gold;
        uint32_t growth;    /* troops gained last tick */
        uint16_t tiles;
        uint16_t minx, maxx, miny, maxy;
        uint32_t allies;    /* bitmask of player ids */
        uint32_t embargo;
        uint32_t nbrs;      /* players we share a land border with (cached) */
        uint32_t reqFrom;   /* pending alliance requests received */
        uint8_t nbrTN;      /* borders unowned land */
        uint8_t big8;       /* large-territory sigmoid, 0..255 */
        uint16_t traitor;   /* ticks left of traitor status */
        uint16_t cityLv;    /* sum of finished city levels */
        uint8_t boats;
        uint8_t nPost;      /* finished defense posts (refreshed every tick) */
        uint8_t target;     /* player marked as target */
        /* AI */
        uint8_t aiWait;
        uint8_t aiEnemy;
        uint8_t aiFlags;
        uint8_t aiTurn;
        int8_t rel[MAXP];
        uint16_t allyUntil[MAXP]; /* tick/16 when alliance with i expires */
        char name[NATION_NAME_LEN];
    };
    uint8_t size_[256];
} player_t;
#define PLAYER_BYTES (offsetof(player_t, name) + NATION_NAME_LEN)

extern player_t g_pl[MAXP];
extern uint8_t g_nplayers; /* highest id in use */

enum { SIZE_NORMAL, SIZE_LARGE };
#define N_MULS 6
extern const uint16_t g_mulQ8[N_MULS]; /* 0.5 1 1.5 2 3 5 in Q8 */

typedef struct {
    uint8_t map;
    uint8_t size;
    uint8_t difficulty;
    uint8_t nations;
    uint8_t tribes;
    uint8_t goldMul;  /* index into g_mulQ8 */
    uint8_t troopMul; /* index into g_mulQ8 */
} game_cfg_t;

extern game_cfg_t g_cfg;
extern uint24_t g_tick;
extern uint8_t g_S;      /* area scale: original tiles per CalcFront tile */
extern uint8_t g_L16;    /* linear scale * 16 */
extern uint16_t g_landLeft; /* land tiles that are not fallout */
extern uint8_t g_winner; /* 0 while running */
extern uint8_t g_mirvs;

extern const uint32_t g_bits[32];
#define BIT(p) (g_bits[p])
/* bit b of a 32-bit player mask, read as one byte: a variable 32-bit shift
 * is a slow library loop on the eZ80 */
#define MASKBIT(m, b) ((((const uint8_t *)&(m))[g_bitLo[(uint8_t)(b)]] & g_bitMask[(uint8_t)(b)]) != 0)
#define ALLIED(a, b) MASKBIT(g_pl[a].allies, b)
#define MASKSET(m, b) (((uint8_t *)&(m))[g_bitLo[(uint8_t)(b)]] |= g_bitMask[(uint8_t)(b)])
#define MASKCLR(m, b) (((uint8_t *)&(m))[g_bitLo[(uint8_t)(b)]] &= (uint8_t)~g_bitMask[(uint8_t)(b)])

uint32_t gold_mul(uint32_t g);
bool can_trade(uint8_t a, uint8_t b);

/* -------------------------------------------------------------- attacks --- */
#define MAX_ATTACKS 40
#define RING 128

typedef struct {
    uint8_t active;
    uint8_t att, def;
    uint8_t retreat;
    uint32_t troops;
    uint24_t budget;
    uint16_t border;   /* estimated frontier size */
    uint8_t head, count;
    uint8_t rescan;
    tile_t ring[RING];
} attack_t;

extern attack_t g_at[MAX_ATTACKS];

/* ---------------------------------------------------------------- units --- */
enum {
    U_NONE,
    U_CITY, U_POST, U_PORT, U_FACTORY, U_SILO, U_SAM, /* structures */
    U_BOAT, U_TRADE, U_WARSHIP,                       /* ships */
    U_ATOM, U_HYDRO, U_MIRV,                          /* missiles */
    U_SHELL, U_TRAIN, U_BOOM,
    U_TYPES
};
#define IS_STRUCT(t) ((t) >= U_CITY && (t) <= U_SAM)
#define IS_SHIP(t) ((t) >= U_BOAT && (t) <= U_WARSHIP)
#define IS_NUKE(t) ((t) >= U_ATOM && (t) <= U_MIRV)
#define IS_STATIONABLE(t) ((t) == U_CITY || (t) == U_PORT || (t) == U_FACTORY)

#define MAX_UNITS 200
#define MAX_PATHS 28
#define PATH_STEPS 640
#define MAX_LEVEL 9

/* Field use per unit type:
 *  structures: hp = built flag, timer = construction / cooldown, aux bit0 = train station,
 *              aux2 = trade-ship spawn rejections (ports)
 *  boat:    dst = water goal, src = landing tile, aux = target player, troops,
 *           hp = 1 when a shell is already on its way
 *  trade:   dst = water goal, tgt = destination port, src = source port, aux = dest owner,
 *           orig = original owner (differs once captured), dist = tiles sailed
 *  warship: dst = patrol centre, tgt = waypoint, hp, level = veterancy, aux = target unit + 1,
 *           aux2 = reload, orig = state, troops = kills, src = trade ships captured
 *  shell:   aux = target unit, aux2 = shooter unit, level = damage, timer = life
 *  nukes:   src = launch, dst = target, timer = flight, timer2 = flight time, aux = victim
 *  train:   src = segment start, tgt = next station, dst = final station,
 *           timer2 = step on segment, aux = segment length, dist = stops                   */
typedef union {
    struct {
        uint8_t type;
        uint8_t owner;
        tile_t pos;
        tile_t dst;
        tile_t src;
        tile_t tgt;
        uint16_t timer;
        uint16_t timer2;
        uint16_t dist;
        uint8_t level;
        uint8_t hp;
        uint8_t path;
        uint8_t aux;
        uint8_t aux2;
        uint8_t orig;
        uint32_t troops;
    };
    uint8_t size_[32];
} unit_t;
#define UNIT_BYTES (offsetof(unit_t, troops) + 4) /* stored in saves */

extern unit_t g_un[MAX_UNITS];
extern uint8_t g_unHi; /* every unit lives below this index */

#define BUILT(u) ((u)->hp != 0)
enum { WS_PATROL, WS_RETREAT, WS_DOCKED };
#define WS_MAXHP(u) (100 + 20 * (u)->level)

/* ---------------------------------------------------------------- events --- */
enum {
    EV_NONE,
    EV_ATTACKED,    /* a attacks you */
    EV_DEAD,        /* a was eliminated (b = killer) */
    EV_ALLY_REQ,    /* a asks for an alliance */
    EV_ALLY_OK,     /* a accepted */
    EV_ALLY_NO,     /* a refused */
    EV_ALLY_END,    /* alliance with a expired */
    EV_BETRAYED,    /* a broke the alliance */
    EV_NUKE_IN,     /* a launched a nuke at you */
    EV_BUILT,       /* structure a finished */
    EV_CAPTURED,    /* you captured a trade ship of b */
    EV_TRADE,       /* trade income, a = gold/1000, b = partner */
    EV_BOAT_LAND,   /* boat of a landed on you */
    EV_SAM_HIT,     /* nuke intercepted */
    EV_LOST_UNIT,   /* your unit of type a was lost to b */
    EV_NOGOLD, EV_BADSPOT, EV_NOSILO, EV_NOROUTE, EV_NOTROOPS, EV_BUSY,
    EV_TARGET,      /* a marked b */
    EV_SHIP_LOST,   /* your trade ship was captured by a */
    EV_SUNK,        /* you sank a ship of type a owned by b */
};

#if defined(HOST) || defined(CF_PERF)
/* statistics for the PC harness and the benchmark: route searches */
#define PATH_STATS
extern unsigned long g_pathPops, g_pathCalls, g_pathFails, g_pathMax, g_pathBfs, g_pathStates, g_pathOverflow;
#endif
#ifdef HOST
extern unsigned long g_boatTry, g_boatFail;
#endif

/* ----------------------------------------------------------- profiling --- */
#ifdef CF_PERF
#include <time.h>
extern clock_t g_perf[12];
#define PERF_START() clock_t perf_t0 = clock()
#define PERF_ADD(i) (g_perf[i] += clock() - perf_t0, perf_t0 = clock())
#define PERF_SKIP() (perf_t0 = clock())
/* finer timers for the benchmark: PROF(i, statement) */
extern clock_t g_prof[16], g_boot[6];
#define PROF(i, stmt)                                                                 \
    do {                                                                              \
        clock_t prof_t0_ = clock();                                                   \
        stmt;                                                                         \
        g_prof[i] += clock() - prof_t0_;                                              \
    } while (0)
#else
#define PROF(i, stmt) stmt
#define PERF_SKIP()
#define PERF_START()
#define PERF_ADD(i)
#endif

/* ----------------------------------------------------------- fixed point --- */
uint16_t fx_log2(uint32_t x);            /* Q8 log2 */
uint32_t fx_exp2(int16_t l);             /* 2^(l/256) */
uint32_t fx_pow(uint32_t x, uint8_t p8); /* x^(p8/256) */
uint16_t rnd(void);
uint16_t rnd_n(uint16_t n); /* 0..n-1 */
tile_t rnd_tile(uint16_t x0, uint16_t w, uint16_t y0, uint16_t h);
void rnd_seed(uint32_t s);
void rnd_get(uint8_t *s); /* 4 bytes of generator state */
void rnd_set(const uint8_t *s);
uint16_t isqrt32(uint32_t v);
uint32_t mulq8(uint32_t x, uint16_t k); /* (x * k) >> 8 */
uint32_t mulq4(uint32_t x, uint16_t k); /* (x * k) >> 4 */
uint16_t tdist(tile_t a, tile_t b);   /* euclidean distance in tiles */
uint16_t mdist(tile_t a, tile_t b);   /* manhattan distance in tiles */
bool near(tile_t a, tile_t b, uint8_t r);

/* ------------------------------------------------------------------ api --- */
/* hooks implemented by the front end (calculator or PC harness) */
void ui_dirty(tile_t t);
void ui_dirty_unit(tile_t t); /* a structure appeared / changed / vanished */
void ui_event(uint8_t type, uint8_t a, uint8_t b);
const uint8_t *plat_map_open(uint8_t idx, uint16_t *size); /* whole map file */
void plat_map_tiles(const uint8_t *data, uint16_t ntiles, uint8_t comp);

/* map.c */
uint8_t map_variants(uint8_t idx);
bool map_load_info(uint8_t idx, uint8_t var);
bool map_load_tiles(uint8_t idx, uint8_t var);
extern bool g_mapClean; /* g_map unchanged since map_load_tiles (clear it after writing tiles) */
void conquer(tile_t t, uint8_t p);
void relinquish(tile_t t);
bool is_shore(tile_t t);          /* land tile touching navigable water */
uint8_t shore_comp(tile_t t);     /* id of navigable water next to t, or 0 */
tile_t water_next_to(tile_t t, uint8_t comp);
void scan_player(uint8_t p);      /* refresh bbox + neighbour info */
bool has_struct(tile_t t);
void st_set(tile_t t, bool on);
tile_t random_land(void);
tile_t random_water_near(tile_t c, uint8_t r, uint8_t comp);
tile_t nearest_own_shore(uint8_t p, tile_t to, uint8_t comp);
uint8_t coast_bodies(uint8_t p);  /* bit c set: p has land next to water body c */
void coast_rebuild(bool scan);
void path_prepare(void);          /* build the coarse route guide now (after loading tiles) */
void path_prepare_begin(void);    /* start it (keeps the last one for the same map) */
bool path_prepare_step(void);     /* one more row of cells; true when done */
void path_ready(void);            /* finish it */
uint8_t path_find(tile_t from, tile_t to);   /* shared, cached; 0xFF on failure */
void path_release(uint8_t slot);
void path_reset(void);
int8_t path_step(uint8_t slot, uint16_t idx); /* dir 0-3, or -1 at the end */
bool path_complete(uint8_t slot);             /* reaches its goal (not truncated) */
tile_t line_point(tile_t a, tile_t b, uint16_t k, uint16_t n); /* k-th of n steps */
uint16_t line_len(tile_t a, tile_t b);

/* game.c */
void game_new(const game_cfg_t *cfg, uint32_t seed);
void game_place_ai(void);
bool game_spawn(uint8_t p, tile_t t);
bool can_spawn_at(tile_t t);
void game_tick(void);
uint8_t add_player(uint8_t type, const char *name);
void kill_player(uint8_t p, uint8_t killer);
void add_troops(uint8_t p, uint32_t n);
void sub_troops(uint8_t p, uint32_t n);
uint16_t land_pct10(uint8_t p);  /* share of the land in 0.1% */
uint16_t land_pct100(uint8_t p); /* share of the land in 0.01% */
uint16_t sim_checksum(void);

/* attack.c */
bool attack_start(uint8_t att, uint8_t def, uint32_t troops, bool removeFromOwner);
void attack_tick(void);
void attack_retreat(uint8_t att, uint8_t def);
bool borders(uint8_t att, uint8_t def);
uint32_t incoming_troops(uint8_t p, uint8_t *from);
bool attacking(uint8_t att, uint8_t def);
void attacks_drop_player(uint8_t p);

/* units.c */
unit_t *new_unit(uint8_t type, uint8_t owner, tile_t pos);
void del_unit(unit_t *u);
uint8_t orig2tiles(uint16_t d);
uint32_t unit_cost(uint8_t p, uint8_t type);
uint16_t levels_of(uint8_t p, uint8_t type);
bool can_build(uint8_t p, uint8_t type, tile_t t);
bool build(uint8_t p, uint8_t type, tile_t t); /* structure, upgrade, warship */
bool launch_nuke(uint8_t p, uint8_t type, tile_t target);
void units_tick(void);
void units_refresh(void);
void unit_on_conquer(tile_t t, uint8_t newOwner);
int16_t unit_at(tile_t t);
bool post_near(uint8_t owner, tile_t t);
void units_drop_player(uint8_t p);
uint8_t count_units(uint8_t p, uint8_t type);
uint8_t sam_range(uint8_t level);
void nuke_radius(uint8_t type, uint8_t *inner, uint8_t *outer);
int16_t find_silo(uint8_t p);

/* ships.c */
bool launch_boat(uint8_t p, tile_t target, uint32_t troops);
bool warship_spawn(uint8_t p, tile_t at);
void warship_order(uint8_t idx, tile_t to);
int16_t ship_near(tile_t t, uint8_t r, uint8_t type, uint8_t owner); /* 0xFF owner = any */
unit_t *port_on(uint8_t p, uint8_t comp, tile_t nearTo);
void port_tick(uint8_t i);
void boat_tick(unit_t *u);
void trade_tick(unit_t *u);
void warship_tick(uint8_t i);
void shell_tick(unit_t *u);
extern uint8_t g_nTrade, g_nTrain;

/* rail.c */
void rail_mark_dirty(void);
void rail_tick(void);
void train_tick(unit_t *u);
void factory_tick(uint8_t i);
bool is_station(const unit_t *u);
void rail_reset(void);
void rail_rebuild(void);
bool rail_any(void); /* any railway on the map */

/* diplo.c */
void ally_request(uint8_t from, uint8_t to);
void ally_answer(uint8_t from, uint8_t to, bool yes);
void ally_break(uint8_t a, uint8_t b);
void diplo_tick(void);
void rel_add(uint8_t p, uint8_t other, int8_t d);

/* save.c: the front end supplies the byte stream (an AppVar on the calculator) */
bool plat_save_write(const void *p, uint16_t n);
bool plat_save_read(void *p, uint16_t n);
bool game_save(void);              /* everything after the front end's own header */
bool game_load(uint8_t mapIdx);    /* map file index of the saved map */

/* ai.c */
void ai_tick(void);
void ai_init_player(uint8_t p);

#endif
