/* CalcFront - an OpenFront-style territory game for the TI-84 Plus CE
 * Game flow, input and menus. */
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

/* ------------------------------------------------------------------ keys --- */
enum {
    K_F1, K_F2, K_F3, K_F4, K_F5, K_CLEAR, K_ENTER, K_PLUS, K_MINUS,
    K_UP, K_DOWN, K_LEFT, K_RIGHT, K_8, K_2, K_4, K_6, K_COUNT
};

static const kb_lkey_t keymap[K_COUNT] = {
    kb_KeyYequ, kb_KeyWindow, kb_KeyZoom, kb_KeyTrace, kb_KeyGraph, kb_KeyClear, kb_KeyEnter,
    kb_KeyAdd, kb_KeySub, kb_KeyUp, kb_KeyDown, kb_KeyLeft, kb_KeyRight,
    kb_Key8, kb_Key2, kb_Key4, kb_Key6};

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
            if (HIT(i)) return i;
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
#ifdef CF_PERF
clock_t g_perf[12];
extern uint8_t g_perfNb, g_perfNm;
static clock_t perfShow[12], perfTotal;
static uint16_t perfFrames;
static uint8_t perfUnits;
static bool perfDirty;

static void perf_draw(void)
{
    static const char *const names[12] = {"eco", "atk", "uni", "ai", "bld", "mov",
                                          "ers", "til", "cur", "ful", "hud", "msc"};
    char buf[64], *p;
    uint8_t i;
    gfx_SetColor(C_BLACK);
    gfx_FillRectangle_NoClip(0, INFO_Y, SCR_W, 240 - INFO_Y);
    p = s_num(buf, (uint32_t)(perfTotal * 1000 / CLOCKS_PER_SEC));
    p = s_cat(p, "ms/100t fr");
    p = s_num(p, perfFrames);
    p = s_cat(p, " u");
    p = s_num(p, perfUnits);
    p = s_cat(p, " b");
    p = s_num(p, g_perfNb);
    p = s_cat(p, " m");
    s_num(p, g_perfNm);
    hud_text(buf, 2, INFO_Y + 1, C_CYAN);
    for (i = 0; i < 12; i++) {
        p = s_cat(buf, names[i]);
        s_num(p, (uint32_t)(perfShow[i] * 1000 / CLOCKS_PER_SEC));
        hud_text(buf, 2 + (i % 6) * 53, KEYS_Y + 1 + (i / 6) * 10, C_WHITE);
    }
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

void ui_event(uint8_t type, uint8_t a, uint8_t b)
{
    char buf[48], *p = buf;
    uint8_t col = C_WHITE;
    switch (type) {
    case EV_ATTACKED:
        p = s_cat(p, g_pl[a].name);
        s_cat(p, " attacks you!");
        col = C_RED;
        break;
    case EV_DEAD:
        if (a == HUMAN || (g_pl[a].type == PT_TRIBE && b != HUMAN)) return;
        p = s_cat(p, g_pl[a].name);
        s_cat(p, " was eliminated");
        col = C_YELLOW;
        break;
    case EV_ALLY_REQ:
        reqPlayer = a;
        reqUntil = clock() + REQ_TIME;
        if (menu != M_REQ && menu < M_REQ) {
            menuBack = menu == M_SHIP ? M_MAIN : menu;
            set_menu(M_REQ);
        }
        p = s_cat(p, g_pl[a].name);
        s_cat(p, " wants an alliance");
        col = C_CYAN;
        break;
    case EV_ALLY_OK:
        p = s_cat(p, g_pl[a].name);
        s_cat(p, " is now your ally");
        col = C_GREEN;
        break;
    case EV_ALLY_NO:
        p = s_cat(p, g_pl[a].name);
        s_cat(p, " refused");
        break;
    case EV_ALLY_END:
        p = s_cat(p, "Alliance ended: ");
        s_cat(p, g_pl[a].name);
        col = C_YELLOW;
        break;
    case EV_BETRAYED:
        p = s_cat(p, g_pl[a].name);
        s_cat(p, " betrayed you!");
        col = C_RED;
        break;
    case EV_NUKE_IN:
        p = s_cat(p, "NUKE incoming from ");
        s_cat(p, g_pl[a].name);
        col = C_RED;
        break;
    case EV_BUILT:
        p = s_cat(p, unitNames[a]);
        s_cat(p, " ready");
        col = C_GREEN;
        break;
    case EV_CAPTURED:
        p = s_cat(p, "Captured a trade ship of ");
        s_cat(p, g_pl[b].name);
        col = C_GREEN;
        break;
    case EV_SHIP_LOST:
        p = s_cat(p, g_pl[a].name);
        s_cat(p, " captured your trade ship");
        col = C_RED;
        break;
    case EV_SUNK:
        p = s_cat(p, "Sank a ");
        p = s_cat(p, unitNames[a]);
        p = s_cat(p, " of ");
        s_cat(p, g_pl[b].name);
        col = C_GREEN;
        break;
    case EV_TRADE:
        p = s_cat(p, "Trade with ");
        p = s_cat(p, g_pl[b].name);
        p = s_cat(p, " +$");
        p = s_num(p, a);
        s_cat(p, "K");
        col = C_YELLOW;
        break;
    case EV_BOAT_LAND:
        p = s_cat(p, g_pl[a].name);
        s_cat(p, " landed on your coast!");
        col = C_RED;
        break;
    case EV_SAM_HIT:
        s_cat(p, "Nuke intercepted by SAM");
        col = C_CYAN;
        break;
    case EV_LOST_UNIT:
        p = s_cat(p, unitNames[a]);
        p = s_cat(p, " lost");
        if (b) {
            p = s_cat(p, " to ");
            s_cat(p, g_pl[b].name);
        }
        col = C_RED;
        break;
    case EV_NOGOLD: s_cat(p, "Not enough gold"); break;
    case EV_BADSPOT: s_cat(p, "Cannot build here"); break;
    case EV_NOSILO: s_cat(p, "No missile silo ready"); break;
    case EV_NOROUTE: s_cat(p, "No sea route"); break;
    case EV_NOTROOPS: s_cat(p, "Not enough troops"); break;
    case EV_BUSY: s_cat(p, "Already 3 boats at sea"); break;
    default: return;
    }
    msg(buf, col);
}

/* ---------------------------------------------------------------- hud bits --- */
/* own warship near the cursor, or -1 */
static int16_t own_ship_at_cursor(void)
{
    return ship_near(TILE(curx, cury), zoom >= 4 ? 1 : (zoom == 2 ? 2 : 4), U_WARSHIP, HUMAN);
}

static void cursor_info(void)
{
    char buf[56], *p = buf;
    tile_t t = TILE(curx, cury);
    uint8_t b = g_map[t], col = C_DIM, k;
    int16_t s = -1;
    static const char *const terr[] = {"", "plains", "highland", "mountain"};
    static const uint8_t shipTypes[] = {U_WARSHIP, U_BOAT, U_TRADE};

    for (k = 0; k < 3 && s < 0; k++) s = ship_near(t, zoom >= 4 ? 0 : 1, shipTypes[k], 0xFF);
    if (s >= 0) {
        const unit_t *u = &g_un[s];
        p = s_cat(p, unitNames[u->type]);
        p = s_cat(p, " - ");
        p = s_cat(p, g_pl[u->owner].name);
        if (u->type == U_WARSHIP) {
            p = s_cat(p, " ");
            p = s_num(p, (uint32_t)u->hp * 100 / WS_MAXHP(u));
            p = s_cat(p, "%");
            if (u->level) {
                p = s_cat(p, " vet ");
                s_num(p, u->level);
            }
        } else if (u->type == U_BOAT) {
            p = s_cat(p, " ");
            s_troops(p, u->troops);
        } else if (u->owner != u->orig)
            s_cat(p, " (captured)");
        hud_info(buf, u->owner == HUMAN ? C_GREEN : C_WHITE);
        return;
    }
    if (!IS_LAND(b))
        s_cat(p, (b & T_COMP) ? "Sea" : "Lake");
    else {
        uint8_t o = b & T_OWNER;
        if (has_struct(t)) {
            int16_t i = unit_at(t);
            if (i >= 0) {
                p = s_cat(p, unitNames[g_un[i].type]);
                if (g_un[i].level > 1) {
                    p = s_cat(p, " L");
                    p = s_num(p, g_un[i].level);
                }
                if (is_station(&g_un[i])) p = s_cat(p, " +rail");
                if (!BUILT(&g_un[i])) p = s_cat(p, " (building)");
                p = s_cat(p, " - ");
            }
        }
        if (!o) {
            p = s_cat(p, (b & T_FALL) ? "Fallout, " : "Free land, ");
            s_cat(p, terr[(b >> 5) & 3]);
        } else {
            const player_t *pl = &g_pl[o];
            p = s_cat(p, pl->name);
            p = s_cat(p, " ");
            p = s_troops(p, pl->troops);
            col = C_WHITE;
            if (o == HUMAN)
                col = C_GREEN;
            else if (ALLIED(HUMAN, o)) {
                s_cat(p, " ally");
                col = C_CYAN;
            } else if (pl->type == PT_TRIBE)
                s_cat(p, " tribe");
            else if (attacking(o, HUMAN) || attacking(HUMAN, o)) {
                s_cat(p, " at war");
                col = C_RED;
            }
        }
    }
    hud_info(buf, col);
}

static uint32_t send_amount(void)
{
    return (g_pl[HUMAN].troops / 100) * g_pl[HUMAN].ratio;
}

static void draw_keys(void)
{
    static char c[5][10], atk[10];
    const char *top[5] = {"", "", "", "", ""};
    const char *bot[5] = {c[0], c[1], c[2], c[3], c[4]};
    uint8_t i, o;
    tile_t t = TILE(curx, cury);
    for (i = 0; i < 5; i++) c[i][0] = 0;
    switch (menu) {
    case M_MAIN:
        if (!IS_LAND(g_map[t]) && own_ship_at_cursor() >= 0) {
            top[0] = "Ship";
            strcpy(c[0], "select");
        } else {
            char *p = s_cat(atk, "Atk ");
            p = s_num(p, g_pl[HUMAN].ratio);
            s_cat(p, "%");
            top[0] = atk;
            fmt_troops(c[0], send_amount());
        }
        top[1] = "Boat"; top[2] = "Build"; top[3] = "Nuke"; top[4] = "More";
        fmt_troops(c[1], send_amount());
        break;
    case M_BUILD:
        if (!IS_LAND(g_map[t])) {
            top[0] = "Warship";
            fmt_num(c[0], unit_cost(HUMAN, U_WARSHIP));
            top[4] = "Back";
        } else {
            static const uint8_t types[4] = {U_CITY, U_PORT, U_FACTORY, U_POST};
            static const char *const names[4] = {"City", "Port", "Factory", "Defense"};
            for (i = 0; i < 4; i++) {
                top[i] = names[i];
                fmt_num(c[i], unit_cost(HUMAN, types[i]));
            }
            top[4] = "More";
        }
        break;
    case M_BUILD2:
        top[0] = "Silo"; top[1] = "SAM"; top[4] = "Back";
        fmt_num(c[0], unit_cost(HUMAN, U_SILO));
        fmt_num(c[1], unit_cost(HUMAN, U_SAM));
        break;
    case M_NUKE:
        top[0] = "Atom"; top[1] = "Hydro"; top[2] = "MIRV"; top[4] = "Back";
        for (i = 0; i < 3; i++) fmt_num(c[i], unit_cost(HUMAN, U_ATOM + i));
        break;
    case M_MORE:
        top[0] = "Diplo"; top[1] = "Atk -"; top[2] = "Atk +"; top[3] = "Ranks"; top[4] = "Speed";
        c[4][0] = 'x';
        c[4][1] = (char)('0' + speed);
        c[4][2] = 0;
        break;
    case M_DIPLO:
        o = IS_LAND(g_map[t]) ? OWNER_OF(t) : 0;
        top[0] = (o && ALLIED(HUMAN, o)) ? "Betray" : "Ally";
        top[1] = "Target";
        top[2] = (o && (g_pl[HUMAN].embargo & BIT(o))) ? "Trade" : "Embargo";
        top[3] = "Retreat";
        top[4] = "Back";
        break;
    case M_SHIP:
        top[0] = "Go here"; top[4] = "Done";
        strcpy(c[0], "patrol");
        break;
    case M_REQ:
        top[0] = "Accept"; top[1] = "Refuse";
        break;
    case M_SPAWN:
        top[0] = "Start"; top[4] = "Quit";
        strcpy(c[0], "here");
        break;
    case M_PAUSE:
        top[0] = "Resume"; top[1] = "Help"; top[4] = "Quit";
        break;
    case M_END:
        top[0] = "Menu"; top[1] = spectate ? "" : "Watch";
        break;
    }
    hud_keys(top, bot);
}

/* ---------------------------------------------------------------- screens --- */
static void help_screen(void)
{
    static const char *const lines[] = {
        "Arrows   move the cursor      8 4 6 2  scroll",
        "+ -      zoom                 CLEAR    back/pause",
        "Y= ... GRAPH  the 5 actions on the bottom bar",
        "",
        "Attack sends Atk% of your troops at the land",
        "under the cursor; Boat crosses the sea. Troops",
        "grow fastest at about half of your maximum.",
        "Cities raise the maximum. Ports trade with other",
        "ports for gold. Factories link nearby cities and",
        "ports by rail; trains earn gold at every stop.",
        "Warships sink boats, fight warships and capture",
        "trade ships: put the cursor on yours, press Ship.",
        "Own 80% of the land to win.",
    };
    uint8_t i;
    hud_panel();
    hud_center("HOW TO PLAY", 6, C_YELLOW);
    for (i = 0; i < sizeof lines / sizeof *lines; i++) hud_text(lines[i], 6, 24 + i * 14, C_WHITE);
    hud_center("press any key", 228, C_DIM);
    wait_key();
}

static void ranks_screen(void)
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
    for (i = 0; i < n && i < 14; i++) {
        uint8_t id = order[i];
        const player_t *pl;
        int y = 34 + i * 13;
        uint16_t pct;
        /* always show the human, on the last line if needed */
        if (i == 13 && g_pl[HUMAN].alive) {
            for (j = 13; j < n; j++)
                if (order[j] == HUMAN) id = HUMAN;
        }
        pl = &g_pl[id];
        pct = land_pct10(id);
        gfx_SetColor(C_FILL + id);
        gfx_FillRectangle_NoClip(8, y, 8, 8);
        hud_text(pl->name, 22, y, id == HUMAN ? C_GREEN : (ALLIED(HUMAN, id) ? C_CYAN : C_WHITE));
        p = s_num(buf, pct / 10);
        *p++ = '.';
        *p++ = (char)('0' + pct % 10);
        *p++ = '%';
        *p = 0;
        hud_text(buf, 172, y, C_WHITE);
        fmt_troops(buf, pl->troops);
        hud_text(buf, 222, y, C_WHITE);
        fmt_num(buf, pl->gold);
        hud_text(buf, 276, y, C_YELLOW);
    }
    p = s_num(buf, n);
    s_cat(p, " players alive - press any key");
    hud_center(buf, 226, C_DIM);
    wait_key();
}

static bool title_screen(void)
{
    uint8_t k;
    hud_panel();
    gfx_SetTextScale(3, 3);
    hud_center("CALCFRONT", 46, C_RED);
    gfx_SetTextScale(1, 1);
    hud_center("conquer the map on your TI-84 Plus CE", 84, C_WHITE);
    if (!nMaps) {
        hud_center("No map files found!", 120, C_YELLOW);
        hud_center("Send the files from maps/calc too.", 136, C_WHITE);
    } else {
        hud_center("Y=  Play", 124, C_GREEN);
        hud_center("WINDOW  How to play", 142, C_WHITE);
    }
    hud_center("GRAPH or CLEAR  Quit", 160, C_WHITE);
    hud_center("A fan remake of OpenFront.io", 208, C_DIM);
    hud_center("maps (c) OpenFront, CC BY-SA 4.0", 222, C_DIM);
    for (;;) {
        k = wait_key();
        if (k == K_F5 || k == K_CLEAR) return false;
        if (k == K_F2) {
            help_screen();
            return title_screen();
        }
        if ((k == K_F1 || k == K_ENTER) && nMaps) return true;
    }
}

static game_cfg_t cfg = {0, SIZE_NORMAL, DIFF_MEDIUM, 10, 10, 1, 1};
static const char *const diffNames[] = {"Easy", "Medium", "Hard", "Impossible"};
static const char *const mulNames[N_MULS] = {"x0.5", "x1", "x1.5", "x2", "x3", "x5"};
#define SETUP_ROWS 7
#define SETUP_H 116

static void setup_draw(uint8_t sel)
{
    char buf[24];
    uint8_t i;
    static const char *const labels[SETUP_ROWS] = {"Map", "Size", "Difficulty", "Nations",
                                                   "Tribes", "Gold", "Troops"};
    gfx_SetColor(C_PANEL);
    gfx_FillRectangle_NoClip(0, 0, SCR_W, SETUP_H);
    hud_center("NEW GAME", 3, C_YELLOW);
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
    hud_panel();
    setup_load_map();
    for (;;) {
        setup_draw(sel);
        keys_wait_release();
        for (;;) {
            keys_scan();
            if (HIT(K_CLEAR)) return false;
            if (HIT(K_F1) || HIT(K_ENTER)) return true;
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
        msg((me->embargo & BIT(o)) ? "Trade stopped" : "Trade resumed", C_WHITE);
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
            ranks_screen();
            r_request_full();
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

/* returns true to go back to the title screen, false to quit */
static bool play(void)
{
    clock_t last = clock();
    bool spawned = false, ended = false, moved;
    const clock_t period = CLOCKS_PER_SEC / 10;

    game_new(&cfg, (uint32_t)clock() * 2654435761UL + rtc_Time());
    game_place_ai();
    r_player_colors();
    vy0 = MAP_Y0;
    vh = MAP_H;
    zoom = 2;
    curx = g_W / 2;
    cury = g_H / 2;
    r_reset();
    r_center(curx, cury);
    spectate = false;
    speed = 1;
    reqUntil = 0;
    gfx_FillScreen(C_BLACK);
    hud_top();
    set_menu(M_SPAWN);
    msg("Choose where to start, then press Y=", C_YELLOW);
    msgFrames = 255;
    keys_wait_release();

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
                        /* drag the cursor along so it stays on screen */
                        int cx = (int)curx + camx - ox, cy = (int)cury + camy - oy;
                        if (cx < 1) cx = 1;
                        if (cx > (int)g_W - 2) cx = g_W - 2;
                        if (cy < 1) cy = 1;
                        if (cy > (int)g_H - 2) cy = g_H - 2;
                        curx = (uint16_t)cx;
                        cury = (uint16_t)cy;
                        r_request_full();
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
                    banner("PAUSED", "", C_YELLOW);
                    infoDirty = keysDirty = true;
                }
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
                banner("PAUSED", "", C_YELLOW);
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
                    memset(g_perf, 0, sizeof g_perf);
                    t0 = now;
                    frames = 0;
                    perfDirty = true;
                }
            }
#endif
#ifdef CF_PERF
            {
                clock_t th = clock();
                if (n && (g_tick & 3) < n) hud_top();
                g_perf[10] += clock() - th;
            }
#else
            if (n && (g_tick & 3) < n) hud_top();
#endif
            if (n && (g_tick & 15) < n) infoDirty = keysDirty = true;

            if (menu == M_REQ && ((long)(clock() - reqUntil) >= 0 || !(g_pl[HUMAN].reqFrom & BIT(reqPlayer)))) {
                if (g_pl[HUMAN].reqFrom & BIT(reqPlayer)) ally_answer(reqPlayer, HUMAN, false);
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
                hud_info(msgBuf, msgColor);
            else
                cursor_info();
        }
        if (keysDirty) {
            keysDirty = false;
            draw_keys();
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
    for (;;) {
        if (!title_screen()) break;
        if (!setup_screen()) continue;
        if (!play()) break;
    }
    gfx_End();
    ti_Delete(TMP_VAR);
    return 0;
}
