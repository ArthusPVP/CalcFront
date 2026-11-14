/* PC harness: runs the CalcFront simulation natively for testing.
 *   cfhost <map-id> <difficulty> <nations> <tribes> <ticks> <seed> [frameEvery] [outdir]
 * Environment: CF_SIZE=1 large map, CF_GOLD / CF_TROOPS = multiplier index (1 = x1),
 *              CF_QUIET=1 one summary line only,
 *              CF_SAVE=n save at tick n, load it back, compare the state, go on.
 * Player 1 (normally the human) is driven by the nation AI here. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stddef.h>
#include "../src/sim/sim.h"

static uint8_t mapmem[65536], stmem[8192];
static sim_mem_t memblock;
uint8_t *g_map = mapmem;
uint8_t *g_stbits = stmem;
sim_mem_t *g_mem = &memblock;
static uint8_t filebuf[140000];
static size_t filelen;
static const char *mapid;
static unsigned long ndirty, nevents[32];


void ui_dirty(tile_t t) { (void)t; ndirty++; }
void ui_dirty_unit(tile_t t) { (void)t; ndirty++; }
void ui_event(uint8_t type, uint8_t a, uint8_t b) { (void)a; (void)b; nevents[type & 31]++; }

const uint8_t *plat_map_open(uint8_t idx, uint16_t *size)
{
    (void)idx;
    if (!filelen) {
        char path[256];
        FILE *f;
        snprintf(path, sizeof path, "maps/pc/%s.cfm", mapid);
        f = fopen(path, "rb");
        if (!f) return 0;
        filelen = fread(filebuf, 1, sizeof filebuf, f);
        fclose(f);
    }
    *size = (uint16_t)(filelen > 65535 ? 65535 : filelen);
    return filebuf;
}

void plat_map_tiles(const uint8_t *data, uint16_t ntiles, uint8_t comp)
{
    if (comp) {
        fprintf(stderr, "compressed maps are calculator-only\n");
        exit(1);
    }
    memcpy(g_map, data, ntiles);
}

/* save stream in memory */
static uint8_t savebuf[70000];
static size_t savelen, savepos;

bool plat_save_write(const void *p, uint16_t n)
{
    if (savelen + n > sizeof savebuf) return false;
    memcpy(savebuf + savelen, p, n);
    savelen += n;
    return true;
}

bool plat_save_read(void *p, uint16_t n)
{
    if (savepos + n > savelen) return false;
    memcpy(p, savebuf + savepos, n);
    savepos += n;
    return true;
}

/* save, reload and check that nothing was lost */
static void save_roundtrip(void)
{
    static uint8_t map0[65536];
    static player_t pl0[MAXP];
    static unit_t un0[MAX_UNITS];
    static attack_t at0[MAX_ATTACKS];
    unsigned i, n = (unsigned)g_W * g_H;
    uint24_t tick0 = g_tick;
    savelen = savepos = 0;
    if (!game_save()) {
        printf("!! save failed\n");
        exit(1);
    }
    memcpy(map0, g_map, n);
    memcpy(pl0, g_pl, sizeof pl0);
    memcpy(un0, g_un, sizeof un0);
    memcpy(at0, g_at, sizeof at0);
    /* wreck the state, as a fresh start would */
    memset(g_pl, 0xA5, sizeof g_pl);
    memset(g_un, 0x5A, sizeof g_un);
    memset(g_at, 0x33, sizeof g_at);
    if (!game_load(0)) {
        printf("!! load failed\n");
        exit(1);
    }
    if (memcmp(map0, g_map, n) || memcmp(pl0, g_pl, sizeof pl0) || g_tick != tick0) {
        printf("!! map, players or tick differ after loading\n");
        exit(1);
    }
    for (i = 0; i < MAX_UNITS; i++) {
        unit_t a = un0[i], b = g_un[i];
        if (a.type == U_NONE && b.type == U_NONE) continue; /* free slots keep no data */
        a.path = b.path = 0;
        if (IS_SHIP(a.type)) a.timer2 = b.timer2 = 0;
        if (IS_STATIONABLE(a.type)) a.aux |= 1, b.aux |= 1; /* railways are rebuilt on load */
        if (memcmp(&a, &b, sizeof a)) {
            printf("!! unit %u differs after loading (hi %u): type %u/%u aux %u/%u level %u/%u hp %u/%u pos %u/%u\n", i, g_unHi,
                   a.type, b.type, a.aux, b.aux, a.level, b.level, a.hp, b.hp, a.pos, b.pos);
            exit(1);
        }
    }
    for (i = 0; i < MAX_ATTACKS; i++)
        if (at0[i].active && memcmp(&at0[i], &g_at[i], offsetof(attack_t, head))) {
            printf("!! attack %u differs after loading\n", i);
            exit(1);
        }
    printf("  save at t=%us: %u bytes, loaded back identical\n", (unsigned)tick0 / 10, (unsigned)savelen);
}

static void pcolor(uint8_t p, uint8_t *rgb)
{
    if (g_pl[p].type == PT_TRIBE) {
        uint8_t g = 110 + (p * 37) % 90;
        rgb[0] = rgb[1] = rgb[2] = g;
    } else {
        rgb[0] = (uint8_t)(60 + (p * 97) % 190);
        rgb[1] = (uint8_t)(60 + (p * 57) % 190);
        rgb[2] = (uint8_t)(60 + (p * 151) % 190);
    }
}

static void dump(const char *dir, int n)
{
    char path[300];
    FILE *f;
    int x, y, i;
    static uint8_t img[512 * 512 * 3];
    static const uint8_t terr[4][3] = {{30, 60, 130}, {150, 190, 110}, {190, 180, 120}, {230, 230, 225}};
    for (y = 0; y < g_H; y++)
        for (x = 0; x < g_W; x++) {
            tile_t t = TILE(x, y);
            uint8_t b = g_map[t], *px = img + (y * g_W + x) * 3;
            if (!IS_LAND(b) || !(b & T_OWNER)) {
                memcpy(px, terr[(b >> 5) & 3], 3);
                if (IS_LAND(b) && (b & T_FALL)) { px[0] = 90; px[1] = 200; px[2] = 60; }
            } else
                pcolor(b & T_OWNER, px);
            if (BITGET(g_railbits, t)) { px[0] = 60; px[1] = 40; px[2] = 30; }
        }
    for (i = 0; i < MAX_UNITS; i++) {
        unit_t *u = &g_un[i];
        uint8_t *px;
        if (u->type == U_NONE) continue;
        px = img + (ty(u->pos) * g_W + tx(u->pos)) * 3;
        if (IS_STRUCT(u->type)) { px[0] = px[1] = px[2] = 0; }
        else if (u->type == U_WARSHIP) { px[0] = 255; px[1] = 0; px[2] = 255; }
        else if (u->type == U_TRAIN) { px[0] = 255; px[1] = 255; px[2] = 0; }
        else if (IS_SHIP(u->type)) { px[0] = 255; px[1] = 255; px[2] = 255; }
        else { px[0] = 255; px[1] = 0; px[2] = 0; }
    }
    snprintf(path, sizeof path, "%s/f%04d.ppm", dir, n);
    f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6 %d %d 255\n", g_W, g_H);
    fwrite(img, 1, (size_t)g_W * g_H * 3, f);
    fclose(f);
}

static void check(void)
{
    static uint16_t cnt[MAXP];
    static uint16_t cityLv[MAXP];
    unsigned i, fall = 0, land = 0;
    memset(cnt, 0, sizeof cnt);
    memset(cityLv, 0, sizeof cityLv);
    for (i = 0; i < (unsigned)g_H * g_W; i++) {
        uint8_t b = g_map[i];
        if (IS_LAND(b)) {
            land++;
            cnt[b & T_OWNER]++;
            if (b & T_FALL) fall++;
            if (b & T_OWNER) {
                uint16_t x = tx(i), y = ty(i);
                player_t *pl = &g_pl[b & T_OWNER];
                if (x < pl->minx || x > pl->maxx || y < pl->miny || y > pl->maxy) {
                    printf("!! tile outside bbox of p%u\n", b & T_OWNER);
                    exit(1);
                }
            }
        } else if (b & ~T_COMP) {
            printf("!! scratch bits left on water tile %u\n", i);
            exit(1);
        }
        if (BITGET(g_stbits, i) && unit_at(i) < 0) {
            printf("!! structure bit without structure at %u\n", i);
            exit(1);
        }
    }
    for (i = 0; i < MAX_UNITS; i++) {
        unit_t *u = &g_un[i];
        if (IS_STRUCT(u->type)) {
            if (!IS_LAND(g_map[u->pos]) || OWNER_OF(u->pos) != u->owner) {
                printf("!! structure %u type %u on wrong tile (owner %u vs %u)\n", i, u->type,
                       OWNER_OF(u->pos), u->owner);
                exit(1);
            }
            if (u->type == U_CITY && BUILT(u)) cityLv[u->owner] += u->level;
        }
        if (IS_SHIP(u->type) && (IS_LAND(g_map[u->pos]) || !(g_map[u->pos] & T_COMP))) {
            printf("!! ship %u type %u on land / frame\n", i, u->type);
            exit(1);
        }
        if (u->type != U_NONE && !g_pl[u->owner].alive && u->type != U_BOOM) {
            printf("!! unit %u type %u of dead player %u\n", i, u->type, u->owner);
            exit(1);
        }
    }
    for (i = 1; i <= g_nplayers; i++) {
        if (cnt[i] != g_pl[i].tiles) {
            printf("!! tile count mismatch p%u: map %u vs %u (tick %u)\n", i, cnt[i],
                   g_pl[i].tiles, (unsigned)g_tick);
            exit(1);
        }
        if (cityLv[i] != g_pl[i].cityLv) {
            printf("!! city level mismatch p%u: %u vs %u\n", i, cityLv[i], g_pl[i].cityLv);
            exit(1);
        }
    }
    {
        /* cached coasts against a full scan */
        static uint8_t seas[MAXP];
        memset(seas, 0, sizeof seas);
        for (i = 0; i < (unsigned)g_H * g_W; i++) {
            uint8_t b = g_map[i], k;
            if (!IS_LAND(b) || !(b & T_OWNER)) continue;
            for (k = 0; k < 4; k++) {
                uint8_t nb = g_map[(tile_t)(i + g_dirofs[k])];
                if (!IS_LAND(nb) && (nb & T_COMP)) seas[b & T_OWNER] |= (uint8_t)(1 << (nb & T_COMP));
            }
        }
        for (i = 1; i <= g_nplayers; i++)
            if (seas[i] != coast_bodies((uint8_t)i)) {
                printf("!! coast of p%u: %02x vs scan %02x\n", i, coast_bodies((uint8_t)i), seas[i]);
                exit(1);
            }
    }
    for (i = 1; i <= g_nplayers; i++) {
        unsigned exact = (unsigned)(((unsigned long)cnt[i] * 10000 + g_landLeft / 2) / g_landLeft);
        unsigned got = land_pct100((uint8_t)i);
        if (got + 1 < exact || got > exact + 1) {
            printf("!! land share p%u: %u vs exact %u\n", i, got, exact);
            exit(1);
        }
    }
    if (land - fall != g_landLeft) {
        printf("!! landLeft mismatch %u vs %u\n", land - fall, g_landLeft);
        exit(1);
    }
}

static void report(void)
{
    int i, j, st = 0, links = 0;
    for (i = 1; i <= g_nplayers; i++) {
        player_t *p = &g_pl[i];
        if (!p->alive || p->tiles < 300) continue;
        printf("  %s nbrs=%08lx tn=%d enemy=%d troops=%u%% borders:", p->name,
               (unsigned long)p->nbrs, p->nbrTN, p->aiEnemy,
               (unsigned)(p->troops / (p->maxTroops / 100 + 1)));
        for (j = 1; j <= g_nplayers; j++)
            if (g_pl[j].alive && j != i && borders((uint8_t)i, (uint8_t)j)) printf(" %s", g_pl[j].name);
        printf("\n");
    }
    for (i = 0; i < MAX_UNITS; i++)
        if (IS_STATIONABLE(g_un[i].type) && is_station(&g_un[i])) st++;
    for (i = 0; i < 8192; i++) links += __builtin_popcount(g_railbits[i]);
    printf("  stations=%d rail tiles=%d\n", st, links);
}

static int envi(const char *n, int def)
{
    const char *v = getenv(n);
    return v ? atoi(v) : def;
}

int main(int argc, char **argv)
{
    game_cfg_t cfg;
    long ticks, seed, every = 0, t, saveAt;
    const char *dir = ".";
    int frame = 0, i, verbose;
    clock_t c0;
    if (argc < 7) {
        fprintf(stderr, "usage: cfhost map diff nations tribes ticks seed [frameEvery] [dir]\n");
        return 2;
    }
    sim_tables();
    mapid = argv[1];
    memset(&cfg, 0, sizeof cfg);
    cfg.difficulty = (uint8_t)atoi(argv[2]);
    cfg.nations = (uint8_t)atoi(argv[3]);
    cfg.tribes = (uint8_t)atoi(argv[4]);
    cfg.size = (uint8_t)envi("CF_SIZE", 0);
    cfg.goldMul = (uint8_t)envi("CF_GOLD", 1);
    cfg.troopMul = (uint8_t)envi("CF_TROOPS", 1);
    ticks = atol(argv[5]);
    seed = atol(argv[6]);
    if (argc > 7) every = atol(argv[7]);
    if (argc > 8) dir = argv[8];
    verbose = getenv("CF_QUIET") == 0;
    saveAt = envi("CF_SAVE", -1);

    if (!map_load_info(0, cfg.size)) {
        fprintf(stderr, "cannot load map %s\n", mapid);
        return 1;
    }
    game_new(&cfg, (uint32_t)seed);
    game_place_ai();
    if (!getenv("CF_HUMAN")) g_pl[HUMAN].type = PT_NATION; /* CF_HUMAN: as the calculator bench */
    for (i = 0; i < 500; i++) {
        tile_t s = random_land();
        if (s != NO_TILE && game_spawn(HUMAN, s)) break;
    }
    if (verbose)
        printf("map %s %ux%u land=%u S=%u L16=%u players=%u\n", g_mi.name, g_W, g_H, g_mi.land,
               g_S, g_L16, g_nplayers);
    if (getenv("CF_HUMAN")) printf("ck 0 %04X\n", sim_checksum());
    c0 = clock();
    for (t = 0; t < ticks; t++) {
        game_tick();
        if (getenv("CF_HUMAN")) {
            static const unsigned at[] = {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000};
            unsigned q;
            for (q = 0; q < 11; q++)
                if (g_tick == at[q]) printf("ck %u %04X\n", at[q], sim_checksum());
        }
        if (t == saveAt) save_roundtrip();
        if ((t & 127) == 0 || t == saveAt) check();
        if (every && t % every == 0) dump(dir, frame++);
        if (verbose && t % 1200 == 0) {
            int alive = 0, at = 0, un[U_TYPES] = {0}, top = 0;
            for (i = 1; i <= g_nplayers; i++) {
                alive += g_pl[i].alive;
                if (g_pl[i].tiles > g_pl[top].tiles) top = i;
            }
            for (i = 0; i < MAX_ATTACKS; i++) at += g_at[i].active;
            for (i = 0; i < MAX_UNITS; i++) un[g_un[i].type]++;
            printf("t=%5lds alive=%2d atk=%2d port=%2d fac=%2d trade=%2d train=%2d war=%2d "
                   "shell=%d top=%-12s %4.1f%% troops=%lu gold=%lu\n",
                   t / 10, alive, at, un[U_PORT], un[U_FACTORY], un[U_TRADE], un[U_TRAIN],
                   un[U_WARSHIP], un[U_SHELL], g_pl[top].name, land_pct10((uint8_t)top) / 10.0,
                   (unsigned long)g_pl[top].troops / 10, (unsigned long)g_pl[top].gold);
        }
        if (g_winner) break;
    }
    check();
    if (every) dump(dir, frame++);
    if (getenv("CF_HUMAN")) printf("checksum at tick %u: %04X\n", (unsigned)g_tick, sim_checksum());
    printf("%s size=%d seed=%ld end t=%lds winner=%s (%.2fs cpu) events:", mapid,
           cfg.size, seed, t / 10, g_winner ? g_pl[g_winner].name : "-",
           (double)(clock() - c0) / CLOCKS_PER_SEC);
    for (i = 1; i < 25; i++) printf(" %lu", nevents[i]);
    printf("\n  routes %lu (failed %lu), tiles searched %lu (max %lu in one), guided %lu (%lu cell states),"
           " overflows %lu, boats %lu (no sea route %lu)\n",
           g_pathCalls, g_pathFails, g_pathPops, g_pathMax, g_pathBfs, g_pathStates, g_pathOverflow, g_boatTry,
           g_boatFail);
    if (verbose) {
        int un[U_TYPES] = {0};
        for (i = 0; i < MAX_UNITS; i++) un[g_un[i].type]++;
        printf("units: city %d post %d port %d factory %d silo %d sam %d boat %d trade %d "
               "warship %d train %d\n",
               un[U_CITY], un[U_POST], un[U_PORT], un[U_FACTORY], un[U_SILO], un[U_SAM],
               un[U_BOAT], un[U_TRADE], un[U_WARSHIP], un[U_TRAIN]);
        for (i = 1; i <= g_nplayers; i++)
            if (g_pl[i].alive)
                printf("  %-13s t%d tiles=%5u troops=%7lu/%7lu (+%lu/s) gold=%9lu\n", g_pl[i].name,
                       g_pl[i].type, g_pl[i].tiles, (unsigned long)g_pl[i].troops / 10,
                       (unsigned long)g_pl[i].maxTroops / 10, (unsigned long)g_pl[i].growth,
                       (unsigned long)g_pl[i].gold);
        report();
    }
    return 0;
}
