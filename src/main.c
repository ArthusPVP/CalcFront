/* CalcFront - an OpenFront-style territory game for the TI-84 Plus CE
 * Game flow, input, menus and saved games. */
#include <tice.h>
#include <keypadc.h>
#include <fileioc.h>
#include <compression.h>
#include <time.h>
#include <string.h>
#include "ce/ui.h"

/* the tile map lives in the unused back half of video memory, followed by
 * the structure bitmap */
uint8_t *g_map = (uint8_t *)0xD52C00;
uint8_t *g_stbits = (uint8_t *)0xD62C00;
/* large scratch buffers live in a temporary RAM variable: static RAM is full */
sim_mem_t *g_mem;
#define TMP_VAR "CFTMP"

static bool alloc_scratch(void)
{
    uint8_t h = ti_Open(TMP_VAR, "w");
    if (!h) return false;
    if (ti_Resize(sizeof(sim_mem_t), h) != (int)sizeof(sim_mem_t)) {
        ti_Close(h);
        ti_Delete(TMP_VAR);
        return false;
    }
    g_mem = ti_GetDataPtr(h);
    ti_Close(h);
    return true;
}

/* Creating, resizing, archiving or deleting a variable that sits before
 * CFTMP in RAM moves CFTMP's data: look its address up again afterwards. */
static void scratch_fix(void)
{
    uint8_t h = ti_Open(TMP_VAR, "r");
    if (h) {
        g_mem = ti_GetDataPtr(h);
        ti_Close(h);
    }
}

/* pro mode is remembered in a one-byte AppVar */
#define SET_VAR "CFSET"

static void settings_load(void)
{
    uint8_t h = ti_Open(SET_VAR, "r"), b = 0;
    if (!h) return;
    if (ti_Read(&b, 1, 1, h) == 1) proMode = b & 1;
    ti_Close(h);
}

static void settings_save(void)
{
    uint8_t h = ti_Open(SET_VAR, "w"), b = proMode;
    if (h) {
        ti_Write(&b, 1, 1, h);
        ti_Close(h);
    }
    scratch_fix();
}

/* ------------------------------------------------------------------ keys --- */
enum {
    K_F1, K_F2, K_F3, K_F4, K_F5, K_CLEAR, K_ENTER, K_PLUS, K_MINUS,
    K_UP, K_DOWN, K_LEFT, K_RIGHT, K_8, K_2, K_4, K_6,
    K_A, K_B, /* MATH and APPS carry the letters A and B */
    K_COUNT
};

static const kb_lkey_t keymap[K_COUNT] = {
    kb_KeyYequ, kb_KeyWindow, kb_KeyZoom, kb_KeyTrace, kb_KeyGraph, kb_KeyClear, kb_KeyEnter,
    kb_KeyAdd, kb_KeySub, kb_KeyUp, kb_KeyDown, kb_KeyLeft, kb_KeyRight,
    kb_Key8, kb_Key2, kb_Key4, kb_Key6, kb_KeyMath, kb_KeyApps};

static uint8_t hold[K_COUNT]; /* frames held, 1 = just pressed */
static clock_t pressT[K_COUNT], repT[K_COUNT], scanT;

/* auto-repeat in real time: frames can be very short when little happens */
#define REP_DELAY (CLOCKS_PER_SEC * 3 / 10)
#define REP_EVERY (CLOCKS_PER_SEC * 7 / 100)
#define FAST_AFTER (CLOCKS_PER_SEC * 7 / 10)

static void keys_scan(void)
{
    uint8_t i;
    kb_Scan();
    scanT = clock();
    for (i = 0; i < K_COUNT; i++) {
        if (kb_IsDown(keymap[i])) {
            if (hold[i] < 250) hold[i]++;
            if (hold[i] == 1) pressT[i] = repT[i] = scanT;
        } else
            hold[i] = 0;
    }
}

#define HIT(k) (hold[k] == 1)

static bool rep(uint8_t k)
{
    if (hold[k] == 1) return true;
    if (!hold[k] || scanT - pressT[k] < REP_DELAY || scanT - repT[k] < REP_EVERY) return false;
    repT[k] = scanT;
    return true;
}

/* held long enough to move the cursor faster */
static bool held_long(uint8_t k) { return hold[k] && scanT - pressT[k] >= FAST_AFTER; }

static void keys_wait_release(void)
{
    do {
        kb_Scan();
    } while (kb_AnyKey());
    memset(hold, 0, sizeof hold);
}

static uint8_t wait_key(void)
{
    uint8_t i;
    keys_wait_release();
    for (;;) {
        keys_scan();
        for (i = 0; i < K_COUNT; i++)
            if (HIT(i)) {
                /* let go first: the key must not also count in the screen
                 * we return to */
                keys_wait_release();
                return i;
            }
    }
}

/* ------------------------------------------------------------------ maps --- */
#define MAX_MAPS 20
static char mapNames[MAX_MAPS][9];
static uint8_t nMaps;

static void find_maps(void)
{
    void *vat = NULL;
    char *name;
    nMaps = 0;
    while ((name = ti_Detect(&vat, "CFM2")) && nMaps < MAX_MAPS) {
        strncpy(mapNames[nMaps], name, 8);
        mapNames[nMaps][8] = 0;
        if (nMaps && !strcmp(name, "CFWORLD")) {
            /* World goes first */
            strcpy(mapNames[nMaps], mapNames[0]);
            strcpy(mapNames[0], "CFWORLD");
        }
        nMaps++;
    }
}

const uint8_t *plat_map_open(uint8_t idx, uint16_t *size)
{
    uint8_t h;
    const uint8_t *p;
    if (idx >= nMaps) return 0;
    h = ti_Open(mapNames[idx], "r");
    if (!h) return 0;
    p = ti_GetDataPtr(h);
    *size = ti_GetSize(h);
    ti_Close(h);
    return p;
}

void plat_map_tiles(const uint8_t *data, uint16_t ntiles, uint8_t comp)
{
    if (comp)
        zx0_Decompress(g_map, data);
    else
        memcpy(g_map, data, ntiles);
}

/* -------------------------------------------------------------- messages --- */
static char msgBuf[48];
static uint8_t msgColor;
static uint8_t msgFrames; /* 0 = no message, 1 = timed, 255 = stays */
static clock_t msgUntil;
#define MSG_TIME (CLOCKS_PER_SEC * 3)
static bool infoDirty, keysDirty;
static uint8_t reqPlayer;
static clock_t reqUntil;
#define REQ_TIME (CLOCKS_PER_SEC * 20)

enum { M_MAIN, M_BUILD, M_BUILD2, M_NUKE, M_MORE, M_DIPLO, M_SHIP, M_REQ, M_SPAWN, M_PAUSE, M_END };
static uint8_t menu, menuBack;
#ifndef CF_PERF
#define PROF_BOOT(i, stmt) stmt
#endif
#ifdef CF_PERF
#define PROF_BOOT(i, stmt)                                                            \
    do {                                                                              \
        clock_t b0_ = clock();                                                        \
        stmt;                                                                         \
        g_boot[i] = clock() - b0_;                                                    \
    } while (0)
clock_t g_perf[12], g_prof[16], g_boot[6];
extern uint8_t g_perfNb, g_perfNm;
static clock_t perfShow[12], perfTotal;
static uint16_t perfFrames;
static uint8_t perfUnits;
static bool perfDirty;

static const char *const perfNames[12] = {"eco", "atk", "uni", "ai", "bld", "mov",
                                          "ers", "til", "cur", "ful", "hud", "msc"};
/* Benchmark (profiling builds only): a fixed game watched at speed x4; at
 * BENCH_TICKS the totals since the start are shown on a full screen. */
#define BENCH_TICKS 2000
static uint32_t benchSum[12], benchFrames;
static uint16_t benchCheck, benchCk[12];
static const uint16_t benchAt[12] = {0, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000};

const uint8_t *next_owned(const uint8_t *p, const uint8_t *end, uint8_t o);
const uint8_t *next_owned_c(const uint8_t *p, const uint8_t *end, uint8_t o);

/* the assembly scan against the C one on random rows: number of mismatches */
static uint16_t asm_selftest(void)
{
    static uint8_t buf[600];
    uint16_t k, bad = 0;
    for (k = 0; k < sizeof buf; k++) buf[k] = (uint8_t)(rnd() & (rnd() & 1 ? 0xE3 : 0x63));
    for (k = 0; k < 3000; k++) {
        uint16_t a = rnd_n(590) + 4, b = a + rnd_n(300);
        uint8_t o = (uint8_t)rnd_n(4);
        if (b > 595) b = 595;
        if (next_owned(buf + a, buf + b, o) != next_owned_c(buf + a, buf + b, o)) bad++;
        if (next_owned(buf + b, buf + a, o) != next_owned_c(buf + b, buf + a, o)) bad++;
    }
    return bad;
}

static void bench_report(void)
{
    char buf[48], *p;
    uint8_t i;
    uint32_t sim = 0;
    hud_panel();
    p = s_cat(buf, "BENCH ");
    p = s_num(p, BENCH_TICKS);
    p = s_cat(p, " ticks, frames ");
    s_num(p, benchFrames);
    hud_center(buf, 6, C_YELLOW);
    for (i = 0; i < 12; i++) {
        uint32_t ms = benchSum[i] * 1000 / CLOCKS_PER_SEC;
        if (i < 4 || i == 11) sim += ms;
        p = s_cat(buf, perfNames[i]);
        p = s_cat(p, "  ");
        p = s_num(p, ms);
        p = s_cat(p, " ms  ");
        p = s_num(p, (i < 4 || i == 11) ? ms * 100 / BENCH_TICKS : ms * 1000 / (benchFrames ? benchFrames : 1));
        s_cat(p, (i < 4 || i == 11) ? " /100t" : " us/fr");
        hud_text(buf, 20, 26 + i * 14, i < 4 || i == 11 ? C_WHITE : C_CYAN);
    }
    p = s_cat(buf, "asm mismatches ");
    s_num(p, asm_selftest());
    hud_center(buf, 226, C_YELLOW);
    p = s_cat(buf, "checksum ");
    for (i = 0; i < 4; i++) *p++ = "0123456789ABCDEF"[(benchCheck >> (12 - 4 * i)) & 15];
    *p = 0;
    hud_center(buf, 214, C_YELLOW);
    p = s_cat(buf, "simulation ");
    p = s_num(p, sim);
    p = s_cat(p, " ms = ");
    p = s_num(p, sim * 10 / BENCH_TICKS);
    s_cat(p, " ms/tick x10");
    hud_center(buf, 200, C_GREEN);
    wait_key();
    {
        static const char *const n2[16] = {"scan_front", "conquer", "calc_max", "recount",
                                           "warship", "port", "boat+trade", "fact+train",
                                           "nat_build", "nat_boat", "navy+nuke", "tribe",
                                           "path_find", "cursor_info", "draw_keys", "hud_top"};
        hud_panel();
        hud_center("BENCH detail (ms over the run)", 4, C_YELLOW);
        {
            unsigned long v[6];
            static const char *const pn[6] = {"routes ", "fails ", "pops ", "max ", "bfs ", "states "};
            v[0] = g_pathCalls;
            v[1] = g_pathFails;
            v[2] = g_pathPops;
            v[3] = g_pathMax;
            v[4] = g_pathBfs;
            v[5] = g_pathStates;
            for (i = 0; i < 6; i++) {
                p = s_cat(buf, pn[i]);
                s_num(p, v[i]);
                hud_text(buf, 12 + (i % 3) * 100, 170 + (i / 3) * 14, C_CYAN);
            }
            for (i = 0; i < 12; i++) {
                uint8_t j;
                p = buf;
                for (j = 0; j < 4; j++) *p++ = "0123456789ABCDEF"[(benchCk[i] >> (12 - 4 * j)) & 15];
                *p = 0;
                hud_text(buf, 12 + (i % 6) * 50, 212 + (i / 6) * 12, C_YELLOW);
            }
            p = s_cat(buf, "overflows ");
            s_num(p, g_pathOverflow);
            hud_text(buf, 12, 198, C_CYAN);
        }
        for (i = 0; i < 16; i++) {
            p = s_cat(buf, n2[i]);
            p = s_cat(p, "  ");
            s_num(p, g_prof[i] * 1000 / CLOCKS_PER_SEC);
            hud_text(buf, 12 + (i / 8) * 160, 24 + (i % 8) * 16, C_WHITE);
        }
        wait_key();
    }
}

static void perf_draw(void)
{
    const char *const *names = perfNames;
    char buf[64], *p;
    uint8_t i;
    gfx_SetColor(C_BLACK);
    gfx_FillRectangle_NoClip(0, 208, SCR_W, 32);
    p = s_num(buf, (uint32_t)(perfTotal * 1000 / CLOCKS_PER_SEC));
    p = s_cat(p, "ms/100t fr");
    p = s_num(p, perfFrames);
    p = s_cat(p, " u");
    p = s_num(p, perfUnits);
    p = s_cat(p, " b");
    p = s_num(p, g_perfNb);
    p = s_cat(p, " m");
    s_num(p, g_perfNm);
    hud_text(buf, 2, 209, C_CYAN);
    for (i = 0; i < 12; i++) {
        p = s_cat(buf, names[i]);
        s_num(p, (uint32_t)(perfShow[i] * 1000 / CLOCKS_PER_SEC));
        hud_text(buf, 2 + (i % 6) * 53, 220 + (i / 6) * 10, C_WHITE);
    }
    hud_invalidate();
}
#endif
static uint8_t speed = 1;
static bool spectate;

static void msg(const char *s, uint8_t color)
{
    strncpy(msgBuf, s, sizeof msgBuf - 1);
    msgColor = color;
    msgFrames = 1;
    msgUntil = clock() + MSG_TIME;
    infoDirty = true;
}

static const char *const unitNames[] = {"", "City", "Defense post", "Port", "Factory",
                                        "Missile silo", "SAM launcher", "Boat", "Trade ship",
                                        "Warship", "Atom bomb", "Hydrogen bomb", "MIRV", "", "Train"};

static void set_menu(uint8_t m)
{
    menu = m;
    keysDirty = true;
    nukeRing = 0;
    if (m != M_SHIP) r_sel = -1;
    if (m == M_NUKE) {
        uint8_t in;
        nuke_radius(U_ATOM, &in, &nukeRing);
    }
}

/* Messages for each event, EV_ATTACKED onward: %a / %b = name of player a / b,
 * %u = unit type a, %k = a thousands, %t = " to " + name of b if b. */
static const char *const evText[] = {
    "%a attacks you!", "%a was eliminated", "%a wants an alliance", "%a is now your ally",
    "%a refused", "Alliance ended: %a", "%a betrayed you!", "NUKE incoming from %a", "%u ready",
    "Captured a trade ship of %b", "Trade with %b +$%kK", "%a landed on your coast!",
    "Nuke intercepted by SAM", "%u lost%t", "Not enough gold", "Cannot build here",
    "No missile silo ready", "No sea route", "Not enough troops", "Already 3 boats at sea", 0,
    "%a captured your trade ship", "Sank a %u of %b"};
static const uint8_t evColor[] = {
    C_RED, C_YELLOW, C_CYAN, C_GREEN, C_WHITE, C_YELLOW, C_RED, C_RED, C_GREEN, C_GREEN,
    C_YELLOW, C_RED, C_CYAN, C_RED, C_WHITE, C_WHITE, C_WHITE, C_WHITE, C_WHITE, C_WHITE, 0,
    C_RED, C_GREEN};

void ui_event(uint8_t type, uint8_t a, uint8_t b)
{
    char buf[48], *p = buf;
    const char *f;
    if (type < EV_ATTACKED || type > EV_SUNK || !(f = evText[type - EV_ATTACKED])) return;
    if (type == EV_DEAD && (a == HUMAN || (g_pl[a].type == PT_TRIBE && b != HUMAN))) return;
    if (type == EV_ALLY_REQ) {
        reqPlayer = a;
        reqUntil = clock() + REQ_TIME;
        if (menu < M_REQ) {
            menuBack = menu == M_SHIP ? M_MAIN : menu;
            set_menu(M_REQ);
        }
    }
    for (; *f; f++) {
        if (*f != '%') {
            *p++ = *f;
            continue;
        }
        switch (*++f) {
        case 'a': p = s_cat(p, g_pl[a].name); break;
        case 'b': p = s_cat(p, g_pl[b].name); break;
        case 'u': p = s_cat(p, unitNames[a]); break;
        case 'k': p = s_num(p, a); break;
        default:
            if (b) {
                p = s_cat(p, " to ");
                p = s_cat(p, g_pl[b].name);
            }
        }
    }
    *p = 0;
    msg(buf, evColor[type - EV_ATTACKED]);
}

/* ---------------------------------------------------------------- hud bits --- */
/* own warship near the cursor, or -1 */
static int16_t own_ship_at_cursor(void)
{
    return ship_near(TILE(curx, cury), zoom >= 4 ? 1 : (zoom == 2 ? 2 : 4), U_WARSHIP, HUMAN);
}

/* the info line under the map: what the cursor is on (pro mode adds
 * terrain, levels, health and the game clock) */
static void cursor_info(void)
{
    char buf[56], clock_[12], *p = buf;
    tile_t t = TILE(curx, cury);
    uint8_t b = g_map[t], col = C_DIM, k;
    int16_t s = -1;
    static const char *const terr[] = {"", "plains", "highland", "mountain"};

    *p = 0;
    if (proMode) s_time(clock_, g_tick / 10);
    {
        /* one pass over the units: warships first, then boats, then trade
         * ships, the closest within reach */
        static const uint8_t rank[3] = {1, 2, 0}; /* boat, trade ship, warship */
        uint8_t r = zoom >= 4 ? 0 : 1, br = 3;
        uint16_t bd = 0xFFFF;
        for (k = 0; k < g_unHi; k++) {
            const unit_t *u = &g_un[k];
            uint8_t rk;
            uint16_t d;
            if (!IS_SHIP(u->type)) continue;
            rk = rank[u->type - U_BOAT];
            if (rk > br) continue;
            d = mdist(u->pos, t);
            if (d > r || (rk == br && d >= bd)) continue;
            br = rk;
            bd = d;
            s = k;
        }
    }
    if (s >= 0) {
        const unit_t *u = &g_un[s];
        p = s_fmt(p, "%s - %s", unitNames[u->type], g_pl[u->owner].name);
        if (u->type == U_BOAT)
            s_fmt(p, " %t", u->troops);
        else if (u->type == U_TRADE && u->owner != u->orig)
            s_cat(p, " (captured)");
        else if (u->type == U_WARSHIP && proMode) {
            p = s_fmt(p, " %n%%", (uint32_t)u->hp * 100 / WS_MAXHP(u));
            if (u->level) s_fmt(p, " vet %n", (uint32_t)u->level);
        }
        col = u->owner == HUMAN ? C_GREEN : C_WHITE;
    } else if (!IS_LAND(b)) {
        if (proMode) s_cat(p, (b & T_COMP) ? "Sea" : "Lake");
    } else {
        static const char *const tag[] = {"", " ally", " tribe", " at war"};
        static const uint8_t tagCol[] = {C_WHITE, C_CYAN, C_WHITE, C_RED};
        uint8_t o = b & T_OWNER, g = 0;
        int16_t i = has_struct(t) ? unit_at(t) : -1;
        if (i >= 0) {
            const unit_t *u = &g_un[i];
            p = s_cat(p, unitNames[u->type]);
            if (proMode) {
                if (u->level > 1) p = s_fmt(p, " L%n", (uint32_t)u->level);
                if (is_station(u)) p = s_cat(p, " +rail");
            }
            p = s_fmt(p, "%s - ", BUILT(u) ? "" : " (building)");
        }
        if (!o)
            s_fmt(p, "%s%s%s", (b & T_FALL) ? "Fallout" : "Free land", proMode ? ", " : "",
                  proMode ? terr[(b >> 5) & 3] : "");
        else {
            const player_t *pl = &g_pl[o];
            if (o == HUMAN)
                col = C_GREEN;
            else {
                g = ALLIED(HUMAN, o) ? 1 : pl->type == PT_TRIBE ? 2 : (attacking(o, HUMAN) || attacking(HUMAN, o)) ? 3 : 0;
                col = tagCol[g];
            }
            s_fmt(p, "%s %t%s", pl->name, pl->troops, tag[g]);
        }
    }
    hud_info(buf, col, proMode ? clock_ : 0);
}

static uint32_t send_amount(void)
{
    return (g_pl[HUMAN].troops / 100) * g_pl[HUMAN].ratio;
}

/* Soft key labels per menu (0 = filled in below) and the unit each key buys,
 * for its price. In normal mode a key is greyed out when you cannot pay for
 * it; pro mode also prints prices and troop counts under the labels. */
static const char *const keyLabel[M_END + 1][5] = {
    [M_MAIN] = {0, "Boat", "Build", "Nuke", "More"},
    [M_BUILD] = {"City", "Port", "Factory", "Defense", "More"},
    [M_BUILD2] = {"Silo", "SAM", "", "", "Back"},
    [M_NUKE] = {"Atom", "Hydro", "MIRV", "", "Back"},
    [M_MORE] = {"Diplo", "Atk -", "Atk +", "Info", 0},
    [M_DIPLO] = {0, "Target", 0, "Retreat", "Back"},
    [M_SHIP] = {"Go here", "", "", "", "Done"},
    [M_REQ] = {"Accept", "Refuse", "", "", ""},
    [M_SPAWN] = {"Start", "", "", "", "Quit"},
    [M_PAUSE] = {"Resume", "Help", "Save", "", "Quit"},
    [M_END] = {"Menu", 0, "", "", ""},
};
static const uint8_t keyUnit[M_END + 1][3] = {
    [M_BUILD] = {U_CITY, U_PORT, U_FACTORY},
    [M_BUILD2] = {U_SILO, U_SAM},
    [M_NUKE] = {U_ATOM, U_HYDRO, U_MIRV},
};

static void draw_keys(void)
{
    static char c[5][10], spd[10];
    const char *top[5], *bot[5];
    uint8_t col[5], i, o;
    tile_t t = TILE(curx, cury);
    bool water = !IS_LAND(g_map[t]), silo = find_silo(HUMAN) >= 0;
    const player_t *me = &g_pl[HUMAN];
    for (i = 0; i < 5; i++) {
        uint8_t u = i < 3 ? keyUnit[menu][i] : 0;
        if (menu == M_BUILD && i == 3) u = U_POST;
        if (menu == M_BUILD && water) u = i ? 0 : U_WARSHIP;
        top[i] = keyLabel[menu][i];
        bot[i] = c[i];
        col[i] = C_WHITE;
        c[i][0] = 0;
        if (u) {
            uint32_t cost = unit_cost(HUMAN, u);
            fmt_num(c[i], cost);
            if (me->gold < cost || (IS_NUKE(u) && !silo)) col[i] = C_DIM;
        }
    }
    switch (menu) {
    case M_MAIN:
        fmt_troops(c[1], send_amount());
        if (water && own_ship_at_cursor() >= 0) {
            top[0] = "Ship";
            strcpy(c[0], "select");
        } else {
            char *p = s_cat(spd, "Atk ");
            p = s_num(p, me->ratio);
            s_cat(p, "%");
            top[0] = spd;
            strcpy(c[0], c[1]);
        }
        break;
    case M_BUILD:
        if (water) {
            top[0] = "Warship";
            top[1] = top[2] = top[3] = "";
            top[4] = "Back";
        }
        break;
    case M_MORE: {
        char *p = s_cat(spd, "Speed x");
        *p++ = (char)('0' + speed);
        *p = 0;
        top[4] = spd;
        if (me->ratio <= 10) col[1] = C_DIM;
        if (me->ratio >= 100) col[2] = C_DIM;
        break;
    }
    case M_DIPLO:
        o = water ? 0 : OWNER_OF(t);
        top[0] = (o && ALLIED(HUMAN, o)) ? "Betray" : "Ally";
        top[2] = (o && MASKBIT(me->embargo, o)) ? "Trade" : "Embargo";
        if (!o || o == HUMAN) col[0] = col[1] = col[2] = C_DIM;
        break;
    case M_SHIP:
        strcpy(c[0], "patrol");
        break;
    case M_REQ:
        col[0] = C_GREEN;
        break;
    case M_SPAWN:
        strcpy(c[0], "here");
        break;
    case M_PAUSE:
        if (!me->alive || g_winner) col[2] = C_DIM;
        break;
    case M_END:
        top[1] = spectate ? "" : "Watch";
        break;
    }
    hud_keys(top, proMode ? bot : 0, col);
}

/* ---------------------------------------------------------------- screens --- */
/* How to play: four pages. A row is led by key caps ('|' between keys), a
 * unit's sprite or a coloured word, followed by its text (\n breaks lines). */
typedef struct {
    uint8_t icon;  /* unit type drawn as the lead, or 0 */
    uint8_t color; /* lead word colour; CAPS = the lead is key caps */
    const char *lead, *text;
} help_t;
#define CAPS 0xFF
#define HELP_PAGES 4

static const char *const helpTitle[HELP_PAGES] = {"Controls", "Grow and win", "Buildings",
                                                   "Ships, nukes, diplomacy"};
static const uint8_t helpStart[HELP_PAGES + 1] = {0, 6, 12, 18, 24};
static const help_t helpRows[] = {
    {0, CAPS, "<|>|^|v", "Move the cursor (hold: faster)"},
    {0, CAPS, "8|4|6|2", "Scroll the map"},
    {0, CAPS, "+|-", "Zoom in and out"},
    {0, CAPS, "enter", "Attack under the cursor"},
    {0, CAPS, "clear", "Back. On the map: the pause\nmenu (resume, help, save, quit)"},
    {0, CAPS, "y=|window|zoom|trace|graph", "\nThe actions on the bottom bar,\nwhich change with each menu"},

    {0, C_GREEN, "Start", "Pick free land away from the\nothers and press Start"},
    {0, C_RED, "Attack", "Free land first, then weak\nneighbours. More > Atk -/+"},
    {0, C_CYAN, "Boat", "Attacks land across the sea"},
    {0, C_WHITE, "Troops", "Grow fastest near half of your\nmaximum; cities raise it"},
    {0, C_YELLOW, "Gold", "Comes in every second, from\ntrade ships and from trains"},
    {0, C_GREEN, "Win", "Own 80% of the land"},

    {U_CITY, C_WHITE, "City", "Raises your troop maximum"},
    {U_PORT, C_WHITE, "Port", "Sends trade ships: gold"},
    {U_FACTORY, C_WHITE, "Factory", "Rails to cities and ports;\ntrains earn gold at stops"},
    {U_POST, C_WHITE, "Defense post", "Nearby land is costly\nto take"},
    {U_SILO, C_WHITE, "Missile silo", "Launches nukes"},
    {U_SAM, C_WHITE, "SAM launcher", "Shoots nukes down"},

    {U_BOAT, C_WHITE, "Boat", "Carries an attack overseas"},
    {U_TRADE, C_WHITE, "Trade ship", "Gold for both ports"},
    {U_WARSHIP, C_WHITE, "Warship", "Sinks boats, steals trade.\nOn it, Ship sends it away"},
    {U_ATOM, C_WHITE, "Nukes", "Atom, Hydrogen, MIRV:\nfire from a missile silo"},
    {0, C_CYAN, "Diplomacy", "More > Diplo: ally, betray,\ntarget, embargo"},
    {0, C_YELLOW, "Info", "More > Info: your nation\nand the leaderboard"},
};

static void help_screen(void)
{
    static const char *const keys[5] = {"< Prev", "Next >", "", "", "Back"};
    uint8_t page = 0, k;
    r_player_colors(); /* sprites in your colours */
    for (;;) {
        char buf[8];
        int y = 34;
        hud_panel();
        hud_center("HOW TO PLAY", 4, C_YELLOW);
        hud_text(helpTitle[page], 8, 18, C_WHITE);
        buf[0] = (char)('1' + page);
        buf[1] = '/';
        buf[2] = (char)('0' + HELP_PAGES);
        buf[3] = 0;
        hud_text(buf, SCR_W - 30, 18, C_DIM);
        gfx_SetColor(C_GREY);
        gfx_HorizLine_NoClip(8, 29, SCR_W - 16);
        for (k = helpStart[page]; k < helpStart[page + 1]; k++) {
            const help_t *h = &helpRows[k];
            const char *t;
            uint8_t lines = 1;
            if (h->color == CAPS) {
                /* key caps, one per '|'-separated name */
                const char *p = h->lead;
                int x = 8;
                while (*p) {
                    uint8_t n = 0;
                    while (*p && *p != '|' && n < sizeof buf - 1) buf[n++] = *p++;
                    buf[n] = 0;
                    if (*p) p++;
                    x += hud_keycap(buf, x, y - 3) + 3;
                }
            } else if (h->icon) {
                r_icon(h->icon, 16, y + 3);
                hud_text(h->lead, 30, y, h->color);
            } else
                hud_text(h->lead, 8, y, h->color);
            hud_lines(h->text, h->icon ? 130 : 104, y, C_WHITE);
            for (t = h->text; *t; t++) lines += *t == '\n';
            y += lines * 11 + 9;
        }
        hud_keys(keys, 0, 0);
        k = wait_key();
        if (k == K_F1 || k == K_LEFT) page = (uint8_t)((page + HELP_PAGES - 1) % HELP_PAGES);
        else if (k == K_F2 || k == K_RIGHT || k == K_ENTER) page = (uint8_t)((page + 1) % HELP_PAGES);
        else if (k == K_CLEAR || k == K_F5) return;
    }
}

/* one "label   value" line of the info screen */
static void info_row(const char *label, const char *value, int y, uint8_t col)
{
    hud_text(label, 8, y, C_DIM);
    hud_text(value, 96, y, col);
}

/* append a list item if it still fits on the line */
static char *list_add(char *buf, char *p, const char *item)
{
    char tmp[40], *q = s_cat(tmp, p == buf ? "" : ", ");
    s_cat(q, item);
    if (p != buf && gfx_GetStringWidth(buf) + gfx_GetStringWidth(tmp) > 214) return p;
    return s_cat(p, tmp);
}

static uint8_t rank_of(uint8_t id)
{
    uint8_t i, r = 1;
    for (i = 1; i <= g_nplayers; i++)
        if (g_pl[i].alive && g_pl[i].tiles > g_pl[id].tiles) r++;
    return r;
}

static int rowY;

static void row(const char *label, const char *value, uint8_t col)
{
    info_row(label, *value ? value : "-", rowY, col);
    rowY += 14;
}

/* everything about your nation that the slim status bar leaves out */
static void you_page(void)
{
    const player_t *me = &g_pl[HUMAN];
    const attack_t *a;
    char buf[64], item[40], *p;
    uint8_t i, n, alive = 0, boats = 0, fronts = 0;
    static const uint8_t types[] = {U_CITY, U_PORT, U_FACTORY, U_POST, U_SILO, U_SAM};
    static const char *const names[2][6] = {{"city", "port", "factory", "post", "silo", "SAM"},
                                            {"cities", "ports", "factories", "posts", "silos", "SAMs"}};

    hud_panel();
    hud_center("YOUR NATION", 6, C_YELLOW);
    rowY = 24;
    s_fmt(buf, "%t / %t   +%t/s", me->troops, me->maxTroops, me->growth * 10);
    row("Troops", buf, C_WHITE);

    for (a = g_at; a < g_at + MAX_ATTACKS; a++) fronts += a->active && a->att == HUMAN;
    for (i = 0; i < g_unHi; i++) boats += g_un[i].type == U_BOAT && g_un[i].owner == HUMAN;
    *buf = 0;
    if (fronts || boats) {
        p = s_fmt(buf, "%t on %n front%s", troops_out(HUMAN), (uint32_t)fronts, fronts == 1 ? "" : "s");
        if (boats) s_fmt(p, ", %n boat%s", (uint32_t)boats, boats == 1 ? "" : "s");
    }
    row("Attacking", buf, C_ORANGE);
    s_fmt(buf, "$%n", me->gold);
    row("Gold", buf, C_YELLOW);

    for (i = 1; i <= g_nplayers; i++) alive += g_pl[i].alive;
    s_fmt(buf, "%p   rank %n of %n", (int)land_pct100(HUMAN), (uint32_t)rank_of(HUMAN), (uint32_t)alive);
    row("Land", buf, C_GREEN);

    p = buf;
    *p = 0;
    for (i = 0; i < sizeof types; i++)
        if ((n = count_units(HUMAN, types[i]))) {
            s_fmt(item, "%n %s", (uint32_t)n, names[n > 1][i]);
            p = list_add(buf, p, item);
        }
    row("Buildings", buf, C_WHITE);
    n = count_units(HUMAN, U_WARSHIP);
    s_fmt(buf, "%n warship%s", (uint32_t)n, n == 1 ? "" : "s");
    row("Navy", buf, C_WHITE);

    /* alliances with the time they have left */
    p = buf;
    *p = 0;
    for (i = 2; i <= g_nplayers; i++)
        if (ALLIED(HUMAN, i)) {
            uint16_t left = (uint16_t)(me->allyUntil[i] - (uint16_t)(g_tick >> 4));
            s_fmt(item, "%s %T", g_pl[i].name, (uint32_t)left * 16 / 10);
            p = list_add(buf, p, item);
        }
    row("Allies", buf, C_CYAN);

    /* who attacks you, with how many troops */
    p = buf;
    *p = 0;
    for (a = g_at; a < g_at + MAX_ATTACKS; a++)
        if (a->active && a->def == HUMAN && !a->retreat) {
            s_fmt(item, "%s %t", g_pl[a->att].name, a->troops);
            p = list_add(buf, p, item);
        }
    row("Attacked by", buf, *buf ? C_RED : C_WHITE);

    p = buf;
    *p = 0;
    for (i = 2; i <= g_nplayers; i++)
        if (g_pl[i].alive && MASKBIT(me->embargo, i)) p = list_add(buf, p, g_pl[i].name);
    row("Embargo", buf, C_WHITE);

    if (me->target && g_pl[me->target].alive) row("Target", g_pl[me->target].name, C_WHITE);
    if (me->traitor) row("Traitor", "allies trust you less", C_RED);
    s_fmt(buf, "%T   speed x%n", (uint32_t)(g_tick / 10), (uint32_t)speed);
    row("Game time", buf, C_WHITE);
}

static void ranks_page(void)
{
    uint8_t order[MAXP], n = 0, i, j;
    char buf[32], *p;
    for (i = 1; i <= g_nplayers; i++)
        if (g_pl[i].alive) order[n++] = i;
    for (i = 0; i + 1 < n; i++)
        for (j = (uint8_t)(i + 1); j < n; j++)
            if (g_pl[order[j]].tiles > g_pl[order[i]].tiles) {
                uint8_t t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
    hud_panel();
    hud_center("LEADERBOARD", 6, C_YELLOW);
    hud_text("Land", 172, 22, C_DIM);
    hud_text("Troops", 222, 22, C_DIM);
    hud_text("Gold", 276, 22, C_DIM);
    for (i = 0; i < n && i < 13; i++) {
        uint8_t id = order[i];
        const player_t *pl;
        int y = 34 + i * 13;
        /* always show the human, on the last line if needed */
        if (i == 12 && g_pl[HUMAN].alive) {
            for (j = 12; j < n; j++)
                if (order[j] == HUMAN) id = HUMAN;
        }
        pl = &g_pl[id];
        gfx_SetColor(C_FILL + id);
        gfx_FillRectangle_NoClip(8, y, 8, 8);
        hud_text(pl->name, 22, y, id == HUMAN ? C_GREEN : (ALLIED(HUMAN, id) ? C_CYAN : C_WHITE));
        s_pct(buf, land_pct100(id), proMode);
        hud_text(buf, 172, y, C_WHITE);
        fmt_troops(buf, pl->troops);
        hud_text(buf, 222, y, C_WHITE);
        fmt_num(buf, pl->gold);
        hud_text(buf, 276, y, C_YELLOW);
    }
    p = s_num(buf, n);
    s_cat(p, " players alive");
    hud_center(buf, 34 + 13 * 13 + 2, C_DIM);
}

/* More > Info: your nation, then the leaderboard */
static void info_screen(void)
{
    static const char *const keys[5] = {"You", "Ranks", "", "", "Back"};
    uint8_t page = 0, k, col[5] = {C_WHITE, C_WHITE, C_WHITE, C_WHITE, C_WHITE};
    for (;;) {
        if (page)
            ranks_page();
        else
            you_page();
        col[0] = page ? C_DIM : C_YELLOW;
        col[1] = page ? C_YELLOW : C_DIM;
        hud_keys(keys, 0, col);
        k = wait_key();
        if (k == K_F1 || k == K_LEFT)
            page = 0;
        else if (k == K_F2 || k == K_RIGHT)
            page = 1;
        else if (k == K_CLEAR || k == K_F5 || k == K_ENTER)
            break;
    }
}

/* ---------------------------------------------------------------- saves --- */
/* A saved game is split over AppVars CFSAVE0, CFSAVE1... of up to 8 KB, each
 * moved to the archive as soon as it is full, so saving needs little free
 * RAM. A part stays in RAM if archiving it would start a garbage collect:
 * the OS would ask on a 16-bit screen, wiping the map kept in video memory. */
#define SAVE_PARTS 8
#define SAVE_PART 8192
static char partName[] = "CFSAVE0";
static uint8_t svH, svPart;
static uint16_t svLeft;
static bool svInRam;
static const char *loadErr;

typedef struct {
    char magic[4];
    char map[9]; /* map AppVar */
    uint8_t zoom, speed;
    uint16_t curx, cury;
} save_hdr_t;

static void part_select(uint8_t i) { partName[6] = (char)('0' + i); }

static void save_delete(void)
{
    uint8_t i;
    for (i = 0; i < SAVE_PARTS; i++) {
        part_select(i);
        ti_Delete(partName);
    }
    scratch_fix();
}

static bool save_exists(void)
{
    uint8_t h;
    part_select(0);
    h = ti_Open(partName, "r");
    if (h) ti_Close(h);
    return h != 0;
}

static void part_close(bool archive)
{
    if (!svH) return;
    if (archive) {
        if (ti_ArchiveHasRoom((uint24_t)ti_GetSize(svH) + 64) && ti_SetArchiveStatus(true, svH))
            ;
        else
            svInRam = true;
    }
    ti_Close(svH);
    svH = 0;
    svPart++;
    scratch_fix();
}

bool plat_save_write(const void *p, uint16_t n)
{
    const uint8_t *s = p;
    while (n) {
        uint16_t k;
        if (!svH) {
            if (svPart >= SAVE_PARTS) return false;
            part_select(svPart);
            svH = ti_Open(partName, "w");
            if (!svH) return false;
            svLeft = SAVE_PART;
        }
        k = n < svLeft ? n : svLeft;
        if (ti_Write(s, k, 1, svH) != 1) return false;
        s += k;
        n -= k;
        svLeft -= k;
        if (!svLeft) part_close(true);
    }
    return true;
}

bool plat_save_read(void *p, uint16_t n)
{
    uint8_t *d = p;
    while (n) {
        size_t got;
        if (!svH) {
            if (svPart >= SAVE_PARTS) return false;
            part_select(svPart);
            svH = ti_Open(partName, "r");
            if (!svH) return false;
        }
        got = ti_Read(d, 1, n, svH);
        d += got;
        n -= (uint16_t)got;
        if (n) part_close(false);
    }
    return true;
}

static bool save_game(void)
{
    save_hdr_t h;
    bool ok;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, "CFS1", 4);
    strcpy(h.map, mapNames[g_cfg.map]);
    h.zoom = zoom;
    h.speed = speed;
    h.curx = curx;
    h.cury = cury;
    save_delete();
    svH = svPart = 0;
    svInRam = false;
    ok = plat_save_write(&h, sizeof h) && game_save();
    part_close(ok);
    if (!ok) save_delete();
    return ok;
}

static bool load_game(void)
{
    save_hdr_t h;
    uint8_t i = 0;
    bool ok = false;
    svH = svPart = 0;
    loadErr = "The saved game is damaged";
    if (plat_save_read(&h, sizeof h) && !memcmp(h.magic, "CFS1", 4)) {
        h.map[8] = 0;
        while (i < nMaps && strcmp(mapNames[i], h.map)) i++;
        if (i == nMaps)
            loadErr = "Its map file is missing";
        else if (game_load(i)) {
            ok = true;
            zoom = (h.zoom == 1 || h.zoom == 2 || h.zoom == 4 || h.zoom == 8) ? h.zoom : 2;
            speed = (h.speed == 2 || h.speed == 4) ? h.speed : 1;
            curx = h.curx > 0 && h.curx < g_W - 1 ? h.curx : g_W / 2;
            cury = h.cury > 0 && h.cury < g_H - 1 ? h.cury : g_H / 2;
        }
    }
    part_close(false);
    if (ok) loadErr = 0;
    return ok;
}

/* the home screen's backdrop: the first map (World if present) with a few
 * nations spread over it, as in a game under way */
static void grow(tile_t t, uint8_t owner, uint8_t size)
{
    tile_t *q = g_mem->vlist;
    uint8_t h = 0, e = 0, i;
    if (!IS_LAND(g_map[t]) || OWNER_OF(t)) return;
    g_mapClean = false;
    g_map[t] |= owner;
    q[e++] = t;
    while (h < e && e < size) {
        tile_t c = q[h++];
        for (i = 0; i < 4 && e < size; i++) {
            tile_t m = (tile_t)(c + g_dirofs[(uint8_t)(i + rnd()) & 3]);
            uint8_t b = g_map[m];
            if (!IS_LAND(b) || (b & T_OWNER) || !(rnd() & 3)) continue;
            g_map[m] = b | owner;
            q[e++] = m;
        }
    }
}

static void title_backdrop(void)
{
    uint8_t k, n;
    memset(g_un, 0, sizeof g_un);
    g_unHi = 0;
    if (!map_load_tiles(0, SIZE_NORMAL)) return;
    memset(g_pl, 0, sizeof g_pl);
    rnd_seed(rtc_Time());
    n = g_mi.nnations < 12 ? g_mi.nnations : 12;
    for (k = 1; k <= n; k++) {
        const map_nation_t *mn = &g_nat[rnd_n(g_mi.nnations)];
        g_pl[k].type = k == HUMAN ? PT_HUMAN : PT_NATION;
        grow(TILE(mn->x, mn->y), k, (uint8_t)(60 + rnd_n(150)));
    }
    r_player_colors();
    vy0 = 4;
    vh = 128;
    r_backdrop();
    /* take the nations off again: the map is then as loaded, and the setup
     * screen can use it without unpacking it a second time */
    {
        uint8_t *p = g_map, *end = g_map + (uint16_t)(g_W * g_H);
        for (; p < end; p++)
            if (IS_LAND(*p)) *p &= (uint8_t)~T_OWNER;
        g_mapClean = true;
    }
}

/* returns 0 to quit, 1 for a new game, 2 to continue the saved one */
static uint8_t title_screen(void)
{
    static const char *const keys[5] = {"New game", "Continue", "Help", "", "Quit"};
    uint8_t k, col[5] = {C_GREEN, C_DIM, C_WHITE, C_WHITE, C_WHITE};
    bool cont = nMaps && save_exists();
    if (cont) col[1] = C_CYAN;
    ui_layout();
    for (;;) {
        hud_panel();
        if (nMaps) PROF_BOOT(0, title_backdrop());
        hud_logo(12);
        hud_center("Conquer the world, one tile at a time", 140, C_WHITE);
        if (!nMaps) {
            hud_center("No map files found!", 70, C_YELLOW);
            hud_center("Send the CF*.8xv files from maps/calc", 86, C_WHITE);
        } else if (loadErr)
            hud_center(loadErr, 160, C_YELLOW);
        else if (cont)
            hud_center("Your saved game is waiting", 160, C_CYAN);
        hud_center("A fan remake of OpenFront.io", 186, C_DIM);
        hud_center("maps (c) OpenFront, CC BY-SA 4.0", 198, C_DIM);
        hud_keys(keys, 0, col);
        for (;;) {
            k = wait_key();
            if (k == K_F5 || k == K_CLEAR) return 0;
            if (k == K_F3) break;
            if ((k == K_F1 || k == K_ENTER) && nMaps) return 1;
            if (k == K_F2 && cont) return 2;
        }
        help_screen();
    }
}

static game_cfg_t cfg = {0, SIZE_NORMAL, DIFF_MEDIUM, 10, 10, 1, 1};
static const char *const diffNames[] = {"Easy", "Medium", "Hard", "Impossible"};
static const char *const mulNames[N_MULS] = {"x0.5", "x1", "x1.5", "x2", "x3", "x5"};
#define SETUP_ROWS 7
#define SETUP_H 116

/* The Konami code on this screen (up up down down left right left right,
 * then B and A: the APPS and MATH keys) switches pro mode on or off. */
static const uint8_t konami[10] = {K_UP, K_UP, K_DOWN, K_DOWN, K_LEFT, K_RIGHT, K_LEFT, K_RIGHT, K_B, K_A};
static uint8_t konamiLast[10];

static bool konami_key(uint8_t k)
{
    memmove(konamiLast, konamiLast + 1, 9);
    konamiLast[9] = k;
    if (memcmp(konamiLast, konami, 10)) return false;
    memset(konamiLast, 0xFF, sizeof konamiLast);
    return true;
}

static void setup_draw(uint8_t sel, const char *note)
{
    char buf[24];
    uint8_t i;
    static const char *const labels[SETUP_ROWS] = {"Map", "Size", "Difficulty", "Nations",
                                                   "Tribes", "Gold", "Troops"};
    gfx_SetColor(C_PANEL);
    gfx_FillRectangle_NoClip(0, 0, SCR_W, SETUP_H);
    hud_center("NEW GAME", 3, C_YELLOW);
    if (proMode) hud_text("PRO", SCR_W - 30, 3, C_CYAN);
    for (i = 0; i < SETUP_ROWS; i++) {
        int y = 16 + i * 13;
        const char *v = buf;
        hud_text(labels[i], 40, y, i == sel ? C_YELLOW : C_DIM);
        switch (i) {
        case 0: v = g_mi.name; break;
        case 1:
            if (g_mi.nvar < 2)
                v = "Normal (only)";
            else {
                char *p = s_cat(buf, cfg.size ? "Large " : "Normal ");
                p = s_num(p, g_W);
                p = s_cat(p, "x");
                s_num(p, g_H);
            }
            break;
        case 2: v = diffNames[cfg.difficulty]; break;
        case 3: fmt_num(buf, cfg.nations); break;
        case 4: fmt_num(buf, cfg.tribes); break;
        case 5: v = mulNames[cfg.goldMul]; break;
        default: v = mulNames[cfg.troopMul]; break;
        }
        if (i == sel) {
            hud_text("<", 140, y, C_YELLOW);
            hud_text(">", 290, y, C_YELLOW);
        }
        hud_text(v, 154, y, C_WHITE);
    }
    if (note)
        hud_center(note, 106, C_CYAN);
    else
        hud_text("Y= start  CLEAR back  arrows change", 24, 106, C_DIM);
}

static void setup_load_map(void)
{
    uint8_t maxN;
    memset(g_un, 0, sizeof g_un);
    g_unHi = 0;
    if (cfg.size >= map_variants(cfg.map)) cfg.size = SIZE_NORMAL;
    if (!map_load_tiles(cfg.map, cfg.size)) return;
    maxN = g_mi.nnations > 24 ? 24 : g_mi.nnations;
    if (cfg.nations > maxN) cfg.nations = maxN;
    if (cfg.tribes > 30 - cfg.nations) cfg.tribes = (uint8_t)(30 - cfg.nations);
    vy0 = SETUP_H;
    vh = 240 - SETUP_H;
    r_preview();
}

static bool setup_screen(void)
{
    uint8_t sel = 0, maxN;
    const char *note = 0;
    hud_panel();
    PROF_BOOT(1, setup_load_map());
    for (;;) {
        setup_draw(sel, note);
        note = 0;
        keys_wait_release();
        for (;;) {
            keys_scan();
            if (HIT(K_CLEAR)) return false;
            if (HIT(K_F1) || HIT(K_ENTER)) return true;
            if (HIT(K_A) || HIT(K_B)) {
                if (!konami_key(HIT(K_A) ? K_A : K_B)) continue;
                proMode = !proMode;
                settings_save();
                note = proMode ? "PRO MODE ON: every number on screen" : "Pro mode off";
                break;
            }
            if (HIT(K_UP) || HIT(K_DOWN) || HIT(K_LEFT) || HIT(K_RIGHT))
                konami_key(HIT(K_UP) ? K_UP : HIT(K_DOWN) ? K_DOWN : HIT(K_LEFT) ? K_LEFT : K_RIGHT);
            if (HIT(K_UP)) { sel = (uint8_t)((sel + SETUP_ROWS - 1) % SETUP_ROWS); break; }
            if (HIT(K_DOWN)) { sel = (uint8_t)((sel + 1) % SETUP_ROWS); break; }
            if (HIT(K_LEFT) || HIT(K_RIGHT)) {
                bool right = HIT(K_RIGHT);
                maxN = g_mi.nnations > 24 ? 24 : g_mi.nnations;
                switch (sel) {
                case 0:
                    cfg.map = (uint8_t)((cfg.map + (right ? 1 : nMaps - 1)) % nMaps);
                    setup_load_map();
                    break;
                case 1:
                    if (g_mi.nvar > 1) {
                        cfg.size ^= 1;
                        setup_load_map();
                    }
                    break;
                case 2:
                    cfg.difficulty = (cfg.difficulty + (right ? 1 : 3)) & 3;
                    break;
                case 3:
                    if (right && cfg.nations < maxN && cfg.nations + cfg.tribes < 30) cfg.nations++;
                    if (!right && cfg.nations) cfg.nations--;
                    break;
                case 4:
                    if (right && cfg.nations + cfg.tribes < 30) cfg.tribes++;
                    if (!right && cfg.tribes) cfg.tribes--;
                    break;
                case 5:
                    cfg.goldMul = (uint8_t)((cfg.goldMul + (right ? 1 : N_MULS - 1)) % N_MULS);
                    break;
                default:
                    cfg.troopMul = (uint8_t)((cfg.troopMul + (right ? 1 : N_MULS - 1)) % N_MULS);
                    break;
                }
                break;
            }
        }
    }
}

/* ----------------------------------------------------------------- actions --- */
static uint8_t owner_at_cursor(void)
{
    uint8_t b = g_map[TILE(curx, cury)];
    return IS_LAND(b) ? (b & T_OWNER) : 0;
}

static void act_attack(bool boat)
{
    tile_t t = TILE(curx, cury);
    uint8_t b = g_map[t], o = b & T_OWNER;
    if (!IS_LAND(b)) {
        msg("Aim at land", C_WHITE);
        return;
    }
    if (o == HUMAN) {
        msg("That is your own land", C_WHITE);
        return;
    }
    if (o && ALLIED(HUMAN, o)) {
        msg("That is an ally (Diplo to betray)", C_WHITE);
        return;
    }
    if (boat) {
        if (launch_boat(HUMAN, t, send_amount())) msg("Boat launched", C_GREEN);
        return;
    }
    if (!borders(HUMAN, o)) {
        msg("No land border - use Boat", C_YELLOW);
        return;
    }
    if (!attack_start(HUMAN, o, send_amount(), true)) msg("Not enough troops", C_WHITE);
}

static void act_diplo(uint8_t k)
{
    uint8_t o = owner_at_cursor();
    player_t *me = &g_pl[HUMAN];
    char buf[48], *p = buf;
    if (k == 3) {
        attack_retreat(HUMAN, o);
        msg("Retreating", C_WHITE);
        return;
    }
    if (!o || o == HUMAN) {
        msg("Aim at another player", C_WHITE);
        return;
    }
    switch (k) {
    case 0:
        if (ALLIED(HUMAN, o)) {
            ally_break(HUMAN, o);
            p = s_cat(p, "You betrayed ");
            s_cat(p, g_pl[o].name);
            msg(buf, C_RED);
        } else {
            msg("Alliance request sent", C_WHITE);
            ally_request(HUMAN, o);
        }
        break;
    case 1:
        me->target = me->target == o ? 0 : o;
        msg(me->target ? "Target marked for your allies" : "Target cleared", C_WHITE);
        break;
    case 2:
        me->embargo ^= BIT(o);
        msg(MASKBIT(me->embargo, o) ? "Trade stopped" : "Trade resumed", C_WHITE);
        break;
    }
    keysDirty = true;
}

static void ship_order(void)
{
    tile_t t = TILE(curx, cury);
    unit_t *u = r_sel >= 0 ? &g_un[r_sel] : 0;
    if (!u || u->type != U_WARSHIP || u->owner != HUMAN) {
        set_menu(M_MAIN);
        return;
    }
    if (IS_LAND(g_map[t]) || (g_map[t] & T_COMP) != (g_map[u->pos] & T_COMP)) {
        msg("Pick water on the same sea", C_YELLOW);
        return;
    }
    warship_order((uint8_t)r_sel, t);
    msg("Warship on its way", C_GREEN);
    set_menu(M_MAIN);
}

static void softkey(uint8_t k)
{
    tile_t t = TILE(curx, cury);
    player_t *me = &g_pl[HUMAN];
    switch (menu) {
    case M_MAIN:
        if (k == 0) {
            int16_t s = IS_LAND(g_map[t]) ? -1 : own_ship_at_cursor();
            if (s >= 0) {
                set_menu(M_SHIP);
                r_sel = s;
                msg("Move the cursor, then Go here", C_CYAN);
            } else
                act_attack(false);
        }
        if (k == 1) act_attack(true);
        if (k == 2) set_menu(M_BUILD);
        if (k == 3) set_menu(M_NUKE);
        if (k == 4) set_menu(M_MORE);
        break;
    case M_BUILD:
        if (!IS_LAND(g_map[t])) {
            if (k == 0 && build(HUMAN, U_WARSHIP, t)) msg("Warship launched", C_GREEN);
            if (k == 4) set_menu(M_MAIN);
        } else if (k == 4)
            set_menu(M_BUILD2);
        else {
            static const uint8_t types[4] = {U_CITY, U_PORT, U_FACTORY, U_POST};
            bool up = has_struct(t);
            if (build(HUMAN, types[k], t)) msg(up ? "Upgraded" : "Construction started", C_GREEN);
        }
        keysDirty = true;
        break;
    case M_BUILD2:
        if (k == 4)
            set_menu(M_BUILD);
        else if (k < 2) {
            bool up = has_struct(t);
            if (build(HUMAN, k ? U_SAM : U_SILO, t)) msg(up ? "Upgraded" : "Construction started", C_GREEN);
            keysDirty = true;
        }
        break;
    case M_NUKE:
        if (k == 4)
            set_menu(M_MAIN);
        else if (k < 3 && launch_nuke(HUMAN, U_ATOM + k, t))
            msg("Missile launched!", C_RED);
        break;
    case M_MORE:
        if (k == 0) set_menu(M_DIPLO);
        if (k == 1 && me->ratio > 10) me->ratio -= 10;
        if (k == 2 && me->ratio < 100) me->ratio += 10;
        if (k == 3) {
            info_screen();
            gfx_FillScreen(C_BLACK);
            r_request_full();
            hud_invalidate();
            hud_top();
            infoDirty = keysDirty = true;
        }
        if (k == 4) speed = speed == 4 ? 1 : speed * 2;
        keysDirty = true;
        break;
    case M_DIPLO:
        if (k == 4)
            set_menu(M_MAIN);
        else
            act_diplo(k);
        break;
    case M_SHIP:
        if (k == 0) ship_order();
        if (k == 4) set_menu(M_MAIN);
        break;
    case M_REQ:
        if (k < 2) {
            ally_answer(reqPlayer, HUMAN, k == 0);
            if (k == 1) msg("Alliance refused", C_WHITE);
            set_menu(menuBack);
        }
        break;
    }
}

/* -------------------------------------------------------------------- play --- */
static void banner(const char *a, const char *b, uint8_t col)
{
    gfx_SetColor(C_BLACK);
    gfx_FillRectangle_NoClip(50, 80, 220, 56);
    gfx_SetColor(col);
    gfx_Rectangle_NoClip(50, 80, 220, 56);
    gfx_SetTextScale(2, 2);
    hud_center(a, 88, col);
    gfx_SetTextScale(1, 1);
    hud_center(b, 116, C_WHITE);
}

static void pause_banner(const char *sub) { banner("PAUSED", sub, C_YELLOW); }

/* Pause menu > Save. Only while you are still in the game. */
static void act_save(void)
{
    if (!g_pl[HUMAN].alive || g_winner) {
        pause_banner("The game is over");
        return;
    }
    pause_banner("Saving...");
    if (!save_game())
        pause_banner("Not enough memory to save");
    else
        pause_banner(svInRam ? "Saved (in RAM: archive full)" : "Game saved");
}

/* returns true to go back to the title screen, false to quit; with resume,
 * the game was just loaded */
static bool play(bool resume)
{
    clock_t last, hudT = 0;
    uint8_t hudN = 0;
    bool spawned = resume, ended = false, moved;
    const clock_t period = CLOCKS_PER_SEC / 10;

    if (!resume) {
#ifdef CF_PERF
        PROF_BOOT(2, game_new(&cfg, 0xC0FFEEUL));
#else
        game_new(&cfg, (uint32_t)clock() * 2654435761UL + rtc_Time());
#endif
        PROF_BOOT(4, game_place_ai());
#ifdef CF_PERF
        PROF_BOOT(3, path_ready());
#endif
        zoom = 2;
        curx = g_W / 2;
        cury = g_H / 2;
        speed = 1;
    }
    r_player_colors();
    ui_layout();
    vy0 = topH;
    vh = (uint8_t)(infoY - topH);
    r_reset();
    r_center(curx, cury);
    spectate = false;
    reqUntil = 0;
    msgFrames = 0;
    gfx_FillScreen(C_BLACK);
    hud_top();
#ifdef CF_PERF
    if (!resume) {
        uint16_t k;
        for (k = 0; k < 500; k++) {
            tile_t t = random_land();
            if (t != NO_TILE && game_spawn(HUMAN, t)) {
                curx = tx(t);
                cury = ty(t);
                break;
            }
        }
        r_center(curx, cury);
        memset(benchSum, 0, sizeof benchSum);
        benchCk[0] = sim_checksum();
        memset(g_prof, 0, sizeof g_prof);
        benchFrames = 0;
        spawned = spectate = true;
        speed = 4;
        resume = true;
    }
#endif
#ifdef CF_PERF
    PROF_BOOT(5, r_frame());
    {
        static const char lbl[6][4] = {"bd", "st", "nw", "pp", "ai", "dr"};
        char t[48], *q = t;
        uint8_t b;
        for (b = 0; b < 6; b++) {
            q = s_cat(q, lbl[b]);
            q = s_num(q, (uint32_t)g_boot[b] * 1000 / CLOCKS_PER_SEC);
            *q++ = ' ';
        }
        *q = 0;
        gfx_SetColor(C_BLACK);
        gfx_FillRectangle_NoClip(0, 100, SCR_W, 12);
        hud_text(t, 4, 102, C_CYAN);
        wait_key(); /* start-up timings (ms), until a key */
        r_request_full();
    }
#endif
    if (resume) {
        set_menu(M_MAIN);
        msg("Welcome back!", C_GREEN);
    } else {
        set_menu(M_SPAWN);
        msg("Choose where to start, then press Y=", C_YELLOW);
        msgFrames = 255;
    }
    keys_wait_release();
    last = clock();

    for (;;) {
        uint8_t i, step;
        keys_scan();
        moved = false;

        /* cursor */
        step = (held_long(K_UP) || held_long(K_DOWN) || held_long(K_LEFT) || held_long(K_RIGHT))
                   ? (zoom == 1 ? 3 : (zoom == 2 ? 2 : 1))
                   : 1;
        if (menu < M_PAUSE) {
            if (rep(K_LEFT)) { curx = curx > step ? curx - step : 1; moved = true; }
            if (rep(K_RIGHT)) { curx = curx + step < g_W - 1 ? curx + step : (uint16_t)(g_W - 2); moved = true; }
            if (rep(K_UP)) { cury = cury > step ? cury - step : 1; moved = true; }
            if (rep(K_DOWN)) { cury = cury + step < g_H - 1 ? cury + step : (uint16_t)(g_H - 2); moved = true; }
            if (moved) r_follow_cursor();

            /* map scrolling */
            {
                int dx = 0, dy = 0, tw = SCR_W / zoom, th = vh / zoom;
                if (rep(K_4)) dx = -tw / 4;
                if (rep(K_6)) dx = tw / 4;
                if (rep(K_8)) dy = -th / 4;
                if (rep(K_2)) dy = th / 4;
                if (dx || dy) {
                    int ox = camx, oy = camy;
                    camx += dx;
                    camy += dy;
                    r_clamp_cam();
                    if (ox != camx || oy != camy) {
                        /* drag the cursor along so it stays on screen; the
                         * renderer slides the picture already drawn */
                        int cx = (int)curx + camx - ox, cy = (int)cury + camy - oy;
                        if (cx < 1) cx = 1;
                        if (cx > (int)g_W - 2) cx = g_W - 2;
                        if (cy < 1) cy = 1;
                        if (cy > (int)g_H - 2) cy = g_H - 2;
                        curx = (uint16_t)cx;
                        cury = (uint16_t)cy;
                        moved = true;
                    }
                }
            }
            if (HIT(K_PLUS) && zoom < 8) {
                zoom <<= 1;
                r_center(curx, cury);
                set_menu(menu);
            }
            if (HIT(K_MINUS) && zoom > 1) {
                zoom >>= 1;
                r_center(curx, cury);
                set_menu(menu);
            }
        }
        if (moved) {
            if (msgFrames != 255) infoDirty = true;
            if (menu == M_MAIN || menu == M_BUILD || menu == M_DIPLO) keysDirty = true;
        }

        /* soft keys */
        for (i = 0; i < 5; i++) {
            if (!HIT(K_F1 + i)) continue;
            if (menu == M_SPAWN) {
                if (i == 0) {
                    if (game_spawn(HUMAN, TILE(curx, cury))) {
                        path_ready(); /* the rest of the route guide, if any */
                        spawned = true;
                        last = clock();
                        set_menu(M_MAIN);
                        msg("Good luck! Attack free land to grow", C_GREEN);
                    } else
                        msg("Pick free land away from others", C_YELLOW);
                }
                if (i == 4) return true;
            } else if (menu == M_PAUSE) {
                if (i == 0) {
                    set_menu(M_MAIN);
                    last = clock();
                    r_request_full();
                }
                if (i == 1) {
                    help_screen();
                    gfx_FillScreen(C_BLACK);
                    hud_top();
                    r_full();
                    pause_banner("");
                    infoDirty = keysDirty = true;
                }
                if (i == 2) act_save();
                if (i == 4) return true;
            } else if (menu == M_END) {
                if (i == 0) return true;
                if (i == 1 && !spectate) {
                    spectate = true;
                    set_menu(M_MAIN);
                    last = clock();
                    r_request_full();
                }
            } else if (!spectate)
                softkey(i);
        }
        if (HIT(K_ENTER) && !spectate) {
            if (menu == M_MAIN) act_attack(false);
            else if (menu == M_SHIP) ship_order();
        }
        if (HIT(K_CLEAR)) {
            if (menu == M_MAIN) {
                set_menu(M_PAUSE);
                r_frame();
                pause_banner("");
            } else if (menu == M_SPAWN || menu == M_END)
                return true;
            else if (menu == M_PAUSE) {
                set_menu(M_MAIN);
                last = clock();
                r_request_full();
            } else if (menu != M_REQ)
                set_menu(M_MAIN);
        }

        /* simulation */
        if (spawned && menu < M_SPAWN) {
            clock_t now = clock();
            uint8_t n = 0;
            while ((clock_t)(now - last) >= period / speed && n < speed) {
                game_tick();
#ifdef CF_PERF
                if (g_tick == BENCH_TICKS) benchCheck = sim_checksum();
                {
                    uint8_t q;
                    for (q = 1; q < 12; q++)
                        if (g_tick == benchAt[q]) benchCk[q] = sim_checksum();
                }
#endif
                last += period / speed;
                n++;
            }
            if ((clock_t)(now - last) > period * 3) last = now;
#ifdef CF_PERF
            {
                static uint16_t frames;
                static clock_t t0;
                frames++;
                if (n && (g_tick % 100) < n) {
                    uint8_t k, units = 0;
                    for (k = 0; k < g_unHi; k++) units += g_un[k].type != U_NONE;
                    perfTotal = now - t0;
                    perfFrames = frames;
                    perfUnits = units;
                    memcpy(perfShow, g_perf, sizeof perfShow);
                    for (k = 0; k < 12; k++) benchSum[k] += g_perf[k];
                    benchFrames += frames;
                    memset(g_perf, 0, sizeof g_perf);
                    t0 = now;
                    frames = 0;
                    perfDirty = true;
                    if (g_tick >= BENCH_TICKS) {
                        bench_report();
                        return true;
                    }
                }
            }
#endif
            /* the status bar every 0.4 s, the keys and info line every 1.6 s
             * (real time: at x4 speed, per tick would be every frame) */
            if (n && (clock_t)(now - hudT) >= CLOCKS_PER_SEC * 4 / 10) {
#ifdef CF_PERF
                clock_t th = clock();
                PROF(15, hud_top());
                g_perf[10] += clock() - th;
#else
                hud_top();
#endif
                hudT = now;
                if (++hudN == 4) {
                    hudN = 0;
                    infoDirty = keysDirty = true;
                }
            }

#ifdef CF_PERF
            if (0) { /* benchmark: leave requests pending, as the PC harness does */
#else
            if (menu == M_REQ && ((long)(clock() - reqUntil) >= 0 || !MASKBIT(g_pl[HUMAN].reqFrom, reqPlayer))) {
#endif
                if (MASKBIT(g_pl[HUMAN].reqFrom, reqPlayer)) ally_answer(reqPlayer, HUMAN, false);
                set_menu(menuBack);
            }
            if (menu == M_SHIP && (r_sel < 0 || g_un[r_sel].type != U_WARSHIP ||
                                   g_un[r_sel].owner != HUMAN))
                set_menu(M_MAIN);
            if (!ended && !spectate && !g_pl[HUMAN].alive) {
                ended = true;
                set_menu(M_END);
                r_frame();
                banner("DEFEAT", "Your nation has fallen", C_RED);
            } else if (g_winner && menu != M_END) {
                spectate = true;
                set_menu(M_END);
                r_frame();
                if (g_winner == HUMAN)
                    banner("VICTORY!", "You rule the world", C_GREEN);
                else
                    banner("GAME OVER", g_pl[g_winner].name, C_YELLOW);
            }
        }

        /* while the player picks a start, build the route guide a row at a time */
        if (menu == M_SPAWN) path_prepare_step();
        if (menu < M_PAUSE) r_frame();
#ifdef CF_PERF
        clock_t hudT0 = clock();
        if (infoDirty || keysDirty) perfDirty = true;
#endif
        if (msgFrames == 1 && (long)(clock() - msgUntil) >= 0) {
            msgFrames = 0;
            infoDirty = true;
        }
        if (infoDirty) {
            infoDirty = false;
            if (msgFrames)
                hud_info(msgBuf, msgColor, 0);
            else
                PROF(13, cursor_info());
        }
        if (keysDirty) {
            keysDirty = false;
            PROF(14, draw_keys());
        }
#ifdef CF_PERF
        g_perf[10] += clock() - hudT0;
        if (perfDirty) {
            perfDirty = false;
            perf_draw();
        }
#endif
    }
}

int main(void)
{
    bool mem = alloc_scratch();
    sim_tables();
    gfx_Begin();
    r_palette();
    gfx_SetTextTransparentColor(255);
    gfx_SetTextBGColor(255);
    if (!mem) {
        hud_panel();
        hud_center("Not enough free RAM", 100, C_YELLOW);
        hud_center("CalcFront needs 21K of free RAM", 120, C_WHITE);
        hud_center("press any key", 150, C_DIM);
        wait_key();
        gfx_End();
        return 0;
    }
    find_maps();
    settings_load();
    for (;;) {
        uint8_t t = title_screen();
        if (!t) break;
        if (t == 2) {
            hud_panel();
            hud_center("Loading...", 110, C_WHITE);
            if (!load_game()) continue;
            if (!play(true)) break;
            continue;
        }
        if (!setup_screen()) continue;
        if (!play(false)) break;
    }
    gfx_End();
    ti_Delete(TMP_VAR);
    return 0;
}
