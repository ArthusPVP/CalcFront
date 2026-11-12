/* text helpers and the fixed parts of the screen: troop bar, info line, soft keys */
#include <string.h>
#include "ui.h"

char *s_cat(char *dst, const char *src)
{
    while (*src) *dst++ = *src++;
    *dst = 0;
    return dst;
}

/* 950 -> "950", 12345 -> "12.3K", 1234567 -> "1.23M" */
void fmt_num(char *dst, uint32_t v)
{
    char unit = 0;
    uint32_t whole, frac = 0;
    uint8_t digits = 0, i;
    char tmp[8];
    if (v >= 1000000) {
        unit = 'M';
        whole = v / 1000000;
        frac = (v % 1000000) / 10000; /* 2 digits */
        digits = whole >= 100 ? 0 : (whole >= 10 ? 1 : 2);
        if (digits == 1) frac /= 10;
    } else if (v >= 1000) {
        unit = 'K';
        whole = v / 1000;
        frac = (v % 1000) / 10;
        digits = whole >= 100 ? 0 : (whole >= 10 ? 1 : 2);
        if (digits == 1) frac /= 10;
    } else
        whole = v;
    i = 0;
    do {
        tmp[i++] = (char)('0' + whole % 10);
        whole /= 10;
    } while (whole);
    while (i) *dst++ = tmp[--i];
    if (digits) {
        *dst++ = '.';
        if (digits == 2) *dst++ = (char)('0' + frac / 10);
        *dst++ = (char)('0' + frac % 10);
    }
    if (unit) *dst++ = unit;
    *dst = 0;
}

/* troops are shown divided by 10, like OpenFront does */
void fmt_troops(char *dst, uint32_t v) { fmt_num(dst, v / 10); }

char *s_num(char *dst, uint32_t v)
{
    fmt_num(dst, v);
    return dst + strlen(dst);
}

char *s_troops(char *dst, uint32_t v)
{
    fmt_troops(dst, v);
    return dst + strlen(dst);
}

void hud_text(const char *s, int x, int y, uint8_t color)
{
    gfx_SetTextFGColor(color);
    gfx_PrintStringXY(s, x, y);
}

void hud_center(const char *s, int y, uint8_t color)
{
    hud_text(s, (SCR_W - (int)gfx_GetStringWidth(s)) / 2, y, color);
}

void hud_panel(void)
{
    gfx_FillScreen(C_PANEL);
}

static void frame_box(int x, int y, int w, int h, uint8_t border, uint8_t fill)
{
    gfx_SetColor(fill);
    gfx_FillRectangle_NoClip(x, y, w, h);
    gfx_SetColor(border);
    gfx_Rectangle_NoClip(x, y, w, h);
}

static void center_in(const char *s, int x, int w, int y, uint8_t color)
{
    hud_text(s, x + (w - (int)gfx_GetStringWidth(s)) / 2, y, color);
}

/* OpenFront-style status bar:
 *  [+growth/s | troops out]  [==== troops / max ====]  [gold | land %] */
void hud_top(void)
{
    char buf[32], *p;
    const player_t *me = &g_pl[HUMAN];
    uint16_t pct = land_pct10(HUMAN);
    uint32_t out = 0, max = me->maxTroops ? me->maxTroops : 1;
    uint8_t i;
    unsigned wCur, wOut;
    const int bx = 60, bw = 178;

    for (i = 0; i < MAX_ATTACKS; i++)
        if (g_at[i].active && g_at[i].att == HUMAN) out += g_at[i].troops;
    for (i = 0; i < g_unHi; i++)
        if (g_un[i].type == U_BOAT && g_un[i].owner == HUMAN) out += g_un[i].troops;

    gfx_SetColor(C_PANEL);
    gfx_FillRectangle_NoClip(0, 0, SCR_W, TOP_H);

    /* growth and troops out */
    frame_box(1, 1, 56, 18, C_GREEN, C_DKGREEN);
    p = s_cat(buf, "+");
    p = s_troops(p, me->growth * 10);
    s_cat(p, "/s");
    center_in(buf, 1, 56, 2, C_GREEN);
    if (out) {
        int w;
        /* crossed swords + troops out attacking */
        s_troops(buf, out);
        w = (int)gfx_GetStringWidth(buf) + 8;
        gfx_SetColor(C_BARATK);
        gfx_Line_NoClip(29 - w / 2, 11, 34 - w / 2, 16);
        gfx_Line_NoClip(34 - w / 2, 11, 29 - w / 2, 16);
        hud_text(buf, 37 - w / 2, 11, C_BARATK);
    } else
        center_in("idle", 1, 56, 11, C_DIM);

    /* troop bar: blue = at home, orange = out attacking */
    frame_box(bx, 1, bw + 2, 18, C_GREY, C_BARBG);
    {
        uint32_t q = max / bw + 1;
        wCur = (unsigned)(me->troops >= max ? bw : me->troops / q);
        wOut = (unsigned)(out >= max ? bw : out / q);
    }
    if (wCur + wOut > (unsigned)bw) wOut = bw - wCur;
    if (wCur) {
        gfx_SetColor(C_BAR);
        gfx_FillRectangle_NoClip(bx + 1, 2, wCur, 16);
    }
    if (wOut) {
        gfx_SetColor(C_BARATK);
        gfx_FillRectangle_NoClip(bx + 1 + wCur, 2, wOut, 16);
    }
    p = s_troops(buf, me->troops);
    p = s_cat(p, " / ");
    s_troops(p, me->maxTroops);
    center_in(buf, bx, bw + 2, 6, C_WHITE);

    /* gold and land share */
    frame_box(241, 1, 78, 18, C_YELLOW, C_PANEL);
    p = s_cat(buf, "$");
    s_num(p, me->gold);
    center_in(buf, 241, 78, 2, C_YELLOW);
    p = s_num(buf, pct / 10);
    *p++ = '.';
    *p++ = (char)('0' + pct % 10);
    s_cat(p, "% land");
    center_in(buf, 241, 78, 11, C_GREEN);
}

void hud_info(const char *s, uint8_t color)
{
    gfx_SetColor(C_BLACK);
    gfx_FillRectangle_NoClip(0, INFO_Y, SCR_W, KEYS_Y - INFO_Y);
    hud_text(s, 2, INFO_Y + 1, color);
}

void hud_keys(const char *const *top, const char *const *bottom)
{
    uint8_t i;
    gfx_SetColor(C_PANEL);
    gfx_FillRectangle_NoClip(0, KEYS_Y, SCR_W, 240 - KEYS_Y);
    gfx_SetColor(C_GREY);
    for (i = 1; i < 5; i++) gfx_VertLine_NoClip(i * 64, KEYS_Y + 1, 18);
    for (i = 0; i < 5; i++) {
        int x0 = i * 64;
        if (top[i] && top[i][0])
            hud_text(top[i], x0 + (64 - (int)gfx_GetStringWidth(top[i])) / 2, KEYS_Y + 2, C_WHITE);
        if (bottom && bottom[i] && bottom[i][0])
            hud_text(bottom[i], x0 + (64 - (int)gfx_GetStringWidth(bottom[i])) / 2, KEYS_Y + 11,
                     C_YELLOW);
    }
}
