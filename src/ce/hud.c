/* text helpers and the fixed parts of the screen: status bar, info line and
 * soft keys. Normal mode keeps them slim; pro mode shows every number.
 * Every part remembers what it shows and is only redrawn when that changes:
 * drawing text is slow, comparing a few bytes is not. */
#include <stdarg.h>
#include <string.h>
#include "ui.h"

bool proMode;
uint8_t topH = 12, infoY = 218, keysY = 228;

/* what each part shows now (bars: widths and text) */
static char shownTop[3][24], shownInfo[56], shownRight[16];
static char shownKey[5][2][10];
static uint8_t shownInfoCol, shownKeyCol[5];
static bool topAll = true, infoAll = true, keysAll = true;

void hud_invalidate(void) { topAll = infoAll = keysAll = true; }

void ui_layout(void)
{
    topH = proMode ? 20 : 12;
    infoY = proMode ? 210 : 218;
    keysY = proMode ? 220 : 228;
    hud_invalidate();
}

/* ---------------------------------------------------------------- text --- */
char *s_cat(char *dst, const char *src)
{
    while (*src) *dst++ = *src++;
    *dst = 0;
    return dst;
}

/* 950 -> "950", 12345 -> "12.3K", 1234567 -> "1.23M" (3 significant
 * digits, truncated). Works on hundredths of the unit in 24-bit integers:
 * 32-bit divisions are long library calls on the eZ80. */
void fmt_num(char *dst, uint32_t v)
{
    unsigned n, whole;
    uint8_t frac, i = 0;
    char unit = 0, tmp[8];
    if (v >= 1000000) {
        n = (unsigned)(v / 10000);
        unit = 'M';
    } else if (v >= 1000) {
        n = (unsigned)v / 10;
        unit = 'K';
    } else
        n = (unsigned)v * 100;
    whole = n / 100;
    frac = (uint8_t)(n % 100);
    do {
        tmp[i++] = (char)('0' + whole % 10);
        whole /= 10;
    } while (whole);
    whole = n / 100;
    while (i) *dst++ = tmp[--i];
    if (unit) {
        if (whole < 100) {
            *dst++ = '.';
            *dst++ = (char)('0' + frac / 10);
            if (whole < 10) *dst++ = (char)('0' + frac % 10);
        }
        *dst++ = unit;
    }
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

/* 1234 -> "12.34%" (two decimals) or "12.3%" */
char *s_pct(char *dst, uint16_t p100, bool two)
{
    uint16_t v = two ? p100 : (uint16_t)((p100 + 5) / 10), div = two ? 100 : 10;
    uint8_t f = (uint8_t)(v % div);
    dst = s_num(dst, v / div);
    *dst++ = '.';
    if (two) *dst++ = (char)('0' + f / 10);
    *dst++ = (char)('0' + f % 10);
    *dst++ = '%';
    *dst = 0;
    return dst;
}

static char *two_digits(char *dst, uint8_t v)
{
    *dst++ = (char)('0' + v / 10);
    *dst++ = (char)('0' + v % 10);
    *dst = 0;
    return dst;
}

char *s_time(char *dst, uint32_t s)
{
    uint16_t m = (uint16_t)(s / 60);
    if (m >= 60) {
        dst = s_num(dst, m / 60);
        *dst++ = ':';
        dst = two_digits(dst, (uint8_t)(m % 60));
    } else
        dst = s_num(dst, m);
    *dst++ = ':';
    return two_digits(dst, (uint8_t)(s % 60));
}

/* A small printf: %s string, %n number, %t troops (/10), %T m:ss time (all
 * three uint32_t), %p 0.01% share with two decimals, %q with one (int). One
 * call per line of text keeps the screens' code small. */
char *s_fmt(char *dst, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    for (; *f; f++) {
        if (*f != '%') {
            *dst++ = *f;
            continue;
        }
        switch (*++f) {
        case '%': *dst++ = '%'; break;
        case 's': dst = s_cat(dst, va_arg(ap, const char *)); break;
        case 'n': dst = s_num(dst, va_arg(ap, uint32_t)); break;
        case 't': dst = s_troops(dst, va_arg(ap, uint32_t)); break;
        case 'T': dst = s_time(dst, va_arg(ap, uint32_t)); break;
        default: dst = s_pct(dst, (uint16_t)va_arg(ap, int), *f == 'p');
        }
    }
    *dst = 0;
    va_end(ap);
    return dst;
}

uint32_t troops_out(uint8_t p)
{
    uint32_t out = 0;
    uint8_t i;
    const attack_t *a;
    for (a = g_at; a < g_at + MAX_ATTACKS; a++)
        if (a->active && a->att == p) out += a->troops;
    for (i = 0; i < g_unHi; i++)
        if (g_un[i].type == U_BOAT && g_un[i].owner == p) out += g_un[i].troops;
    return out;
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
    hud_invalidate();
}

static void box(int x, int y, int w, int h, uint8_t fill)
{
    gfx_SetColor(fill);
    gfx_FillRectangle_NoClip(x, y, w, h);
}

static void frame_box(int x, int y, int w, int h, uint8_t border, uint8_t fill)
{
    box(x, y, w, h, fill);
    gfx_SetColor(border);
    gfx_Rectangle_NoClip(x, y, w, h);
}

static void center_in(const char *s, int x, int w, int y, uint8_t color)
{
    hud_text(s, x + (w - (int)gfx_GetStringWidth(s)) / 2, y, color);
}

/* true (and remember s) when s differs from what the part shows */
static bool changed(char *shown, const char *s, bool all)
{
    if (!all && !strcmp(shown, s)) return false;
    strcpy(shown, s);
    return true;
}

/* ---------------------------------------------------------------- logo --- */
/* "CALCFRONT" in a bold 7x9 block font, one byte per row (bit 6 = left),
 * drawn 4x with a drop shadow, a dark edge and a light-to-dark gradient */
static const uint8_t logoFont[6][9] = {
    {0x3F, 0x7F, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7F, 0x3F}, /* C */
    {0x1C, 0x3E, 0x63, 0x63, 0x7F, 0x7F, 0x63, 0x63, 0x63}, /* A */
    {0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7F, 0x7F}, /* L */
    {0x7F, 0x7F, 0x60, 0x60, 0x7E, 0x7E, 0x60, 0x60, 0x60}, /* F */
    {0x7E, 0x7F, 0x63, 0x63, 0x7E, 0x7C, 0x6C, 0x66, 0x63}, /* R */
    {0x3E, 0x7F, 0x63, 0x63, 0x63, 0x63, 0x63, 0x7F, 0x3E}, /* O */
};
static const uint8_t logoN[9] = {0x63, 0x73, 0x7B, 0x7F, 0x6F, 0x67, 0x63, 0x63, 0x63};
static const uint8_t logoT[9] = {0x7F, 0x7F, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C};

void hud_logo(int y)
{
    static const char word[] = {0, 1, 2, 0, 3, 4, 5, 6, 7}; /* C A L C F R O N T */
    uint8_t pass, l, r, c;
    const int s = 4, x0 = (SCR_W - (9 * 8 - 1) * s) / 2;
    for (pass = 0; pass < 3; pass++)
        for (l = 0; l < 9; l++) {
            const uint8_t *g = word[l] == 6 ? logoN : word[l] == 7 ? logoT : logoFont[(uint8_t)word[l]];
            for (r = 0; r < 9; r++)
                for (c = 0; c < 7; c++) {
                    /* one rectangle per run of lit pixels in the row */
                    int x = x0 + (l * 8 + c) * s, yy = y + r * s, n = 0;
                    while (c + n < 7 && (g[r] & (0x40 >> (c + n)))) n++;
                    if (!n) continue;
                    c = (uint8_t)(c + n - 1);
                    n *= s;
                    if (pass == 0)
                        box(x + 3, yy + 4, n, s, C_SHADOW);
                    else if (pass == 1)
                        box(x - 2, yy - 2, n + 4, s + 4, C_LOGO0);
                    else
                        box(x, yy, n, s, r < 3 ? C_LOGO1 : r < 6 ? C_LOGO2 : C_LOGO3);
                }
        }
}

int hud_keycap(const char *s, int x, int y)
{
    int w = (int)gfx_GetStringWidth(s) + 8;
    frame_box(x, y, w, 13, C_GREY, C_KEYCAP);
    gfx_SetColor(C_PANEL); /* round the corners */
    gfx_SetPixel(x, y);
    gfx_SetPixel(x + w - 1, y);
    gfx_SetPixel(x, y + 12);
    gfx_SetPixel(x + w - 1, y + 12);
    hud_text(s, x + 4, y + 3, C_WHITE);
    return w;
}

void hud_lines(const char *s, int x, int y, uint8_t color)
{
    char buf[56];
    while (*s) {
        uint8_t n = 0;
        while (*s && *s != '\n' && n < sizeof buf - 1) buf[n++] = *s++;
        buf[n] = 0;
        hud_text(buf, x, y, color);
        y += 11;
        if (*s == '\n') s++;
    }
}

/* ---------------------------------------------------------- status bar --- */
/* troop bar: blue = at home, orange = out attacking */
static void troop_bar(int x, int y, int w, int h, unsigned wCur, unsigned wOut, const char *text, int ty)
{
    frame_box(x, y, w + 2, h, C_GREY, C_BARBG);
    if (wCur) box(x + 1, y + 1, wCur, h - 2, C_BAR);
    if (wOut) box(x + 1 + wCur, y + 1, wOut, h - 2, C_BARATK);
    center_in(text, x, w + 2, ty, C_WHITE);
}

void hud_top(void)
{
    char key[24], buf[24], *p;
    const player_t *me = &g_pl[HUMAN];
    uint16_t pct = land_pct100(HUMAN);
    uint32_t out = troops_out(HUMAN), max = me->maxTroops ? me->maxTroops : 1, q;
    unsigned wCur, wOut;
    const int bw = proMode ? 178 : 196;

    q = max / bw + 1;
    wCur = (unsigned)(me->troops >= max ? bw : me->troops / q);
    wOut = (unsigned)(out >= max ? bw : out / q);
    if (wCur + wOut > (unsigned)bw) wOut = bw - wCur;
    if (topAll) box(0, 0, SCR_W, topH, C_PANEL);

    /* the troop bar is keyed by its widths and its text */
    p = buf;
    *p++ = (char)(wCur + 1);
    *p++ = (char)(wOut + 1);
    p = s_troops(p, me->troops);
    p = s_cat(p, " / ");
    s_troops(p, me->maxTroops);

    if (!proMode) {
        /* [==== troops / max ====]  $gold  land% */
        if (changed(shownTop[0], buf, topAll)) troop_bar(1, 1, bw, 10, wCur, wOut, buf + 2, 2);
        p = s_cat(key, "$");
        s_num(p, me->gold);
        if (changed(shownTop[1], key, topAll)) {
            box(201, 0, 66, topH, C_PANEL);
            hud_text(key, 202, 2, C_YELLOW);
        }
        s_pct(key, pct, false);
        if (changed(shownTop[2], key, topAll)) {
            box(268, 0, SCR_W - 268, topH, C_PANEL);
            hud_text(key, SCR_W - 2 - (int)gfx_GetStringWidth(key), 2, C_GREEN);
        }
        topAll = false;
        return;
    }

    /* pro: [+growth/s | troops out]  [==== troops / max ====]  [$gold | land%] */
    if (changed(shownTop[0], buf, topAll)) troop_bar(60, 1, bw, 18, wCur, wOut, buf + 2, 6);
    p = s_cat(key, "+");
    p = s_troops(p, me->growth * 10);
    p = s_cat(p, "/s");
    *p++ = '|';
    if (out) s_troops(p, out);
    else *p = 0;
    if (changed(shownTop[1], key, topAll)) {
        frame_box(1, 1, 56, 18, C_GREEN, C_DKGREEN);
        p = strchr(key, '|');
        *p = 0;
        center_in(key, 1, 56, 2, C_GREEN);
        if (out) {
            int w = (int)gfx_GetStringWidth(p + 1) + 8;
            /* crossed swords + troops out attacking */
            gfx_SetColor(C_BARATK);
            gfx_Line_NoClip(29 - w / 2, 11, 34 - w / 2, 16);
            gfx_Line_NoClip(34 - w / 2, 11, 29 - w / 2, 16);
            hud_text(p + 1, 37 - w / 2, 11, C_BARATK);
        } else
            center_in("idle", 1, 56, 11, C_DIM);
        *p = '|';
    }
    p = s_cat(key, "$");
    p = s_num(p, me->gold);
    *p++ = '|';
    s_pct(p, pct, true);
    if (changed(shownTop[2], key, topAll)) {
        frame_box(241, 1, 78, 18, C_YELLOW, C_PANEL);
        p = strchr(key, '|');
        *p = 0;
        center_in(key, 241, 78, 2, C_YELLOW);
        center_in(p + 1, 241, 78, 11, C_GREEN);
        *p = '|';
    }
    topAll = false;
}

/* --------------------------------------------------------------- info --- */
void hud_info(const char *s, uint8_t color, const char *right)
{
    if (!right) right = "";
    if (!infoAll && color == shownInfoCol && !strcmp(s, shownInfo) && !strcmp(right, shownRight)) return;
    infoAll = false;
    shownInfoCol = color;
    strncpy(shownInfo, s, sizeof shownInfo - 1);
    strncpy(shownRight, right, sizeof shownRight - 1);
    box(0, infoY, SCR_W, keysY - infoY, C_BLACK);
    hud_text(s, 2, infoY + 1, color);
    if (*right) {
        int x = SCR_W - 2 - (int)gfx_GetStringWidth(right);
        if (x > (int)gfx_GetStringWidth(s) + 8) hud_text(right, x, infoY + 1, C_DIM);
    }
}

/* --------------------------------------------------------------- keys --- */
/* five soft keys above Y= WINDOW ZOOM TRACE GRAPH: a label, and in pro mode
 * a value under it; colors: label colour per key (NULL = white) */
void hud_keys(const char *const *top, const char *const *bottom, const uint8_t *colors)
{
    uint8_t i, h = (uint8_t)(240 - keysY);
    if (keysAll) {
        box(0, keysY, SCR_W, h, C_PANEL);
        gfx_SetColor(C_GREY);
        for (i = 1; i < 5; i++) gfx_VertLine_NoClip(i * 64, keysY + 1, h - 2);
    }
    for (i = 0; i < 5; i++) {
        const char *t = top[i] ? top[i] : "", *b = bottom && bottom[i] ? bottom[i] : "";
        uint8_t col = colors ? colors[i] : C_WHITE;
        int x0 = i * 64;
        if (!keysAll && col == shownKeyCol[i] && !strcmp(t, shownKey[i][0]) && !strcmp(b, shownKey[i][1]))
            continue;
        shownKeyCol[i] = col;
        strncpy(shownKey[i][0], t, 9);
        strncpy(shownKey[i][1], b, 9);
        box(x0 + 1, keysY, 63, h, C_PANEL);
        if (*t) hud_text(t, x0 + (64 - (int)gfx_GetStringWidth(t)) / 2, keysY + 2, col);
        if (*b) hud_text(b, x0 + (64 - (int)gfx_GetStringWidth(b)) / 2, keysY + 11, C_YELLOW);
    }
    keysAll = false;
}
