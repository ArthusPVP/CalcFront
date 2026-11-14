/* map renderer: full redraws, incremental repaints, sprites, railways, cursor
 *
 * Everything is drawn straight into video memory (single buffer). Each frame:
 *   0. if the camera moved, slide the picture already on screen (a block
 *      copy) and paint only the strips that came into view
 *   1. repaint the map under sprites that moved, the cursor and changed
 *      buildings (paint_rect: row by row, one colour lookup per tile span)
 *   2. repaint tiles whose owner changed (and their neighbours: borders)
 *   3. draw every visible sprite from pre-built masks */
#include <string.h>
#include <time.h>
#include "ui.h"

int camx, camy;
uint8_t zoom = 2;
uint16_t curx, cury;
uint8_t vy0 = 12, vh = 206;
uint8_t nukeRing;
int16_t r_sel = -1;

#define VRAM ((uint8_t *)0xD40000)

static uint8_t colLUT[256];
static uint8_t line[SCR_W], rline[SCR_W];
static uint8_t zsh;        /* log2(zoom) */
static uint8_t railLo, railN; /* rail mark inside big tiles: offset and size */

#define DQ 1024
static tile_t dq[DQ];
static uint16_t dqh, dqn;
#define SQ 32
static tile_t sq[SQ];
static uint8_t sqn;
static bool fullReq;
static int drawnCamX, drawnCamY; /* camera of the picture in video memory */
static bool railOn;              /* any railway to draw */

static tile_t drawnAt[MAX_UNITS];
static uint8_t drawnType[MAX_UNITS];
static uint8_t drawnLevel[MAX_UNITS];
static int curSX = -100, curSY, curRing;

/* ----------------------------------------------------------- palette --- */
static void pal(uint8_t i, uint8_t r, uint8_t g, uint8_t b) { gfx_palette[i] = gfx_RGBTo1555(r, g, b); }

static const uint8_t nationRGB[20][3] = {
    {255, 150, 0}, {250, 220, 40}, {170, 60, 200}, {0, 190, 190}, {255, 105, 180},
    {120, 70, 30}, {60, 120, 255}, {20, 130, 60}, {190, 255, 90}, {130, 0, 60},
    {255, 190, 150}, {90, 90, 200}, {0, 230, 130}, {200, 120, 255}, {160, 160, 40},
    {255, 80, 120}, {70, 200, 255}, {200, 90, 40}, {110, 40, 140}, {230, 230, 170}};

static void build_masks(void);

/* fixed colours C_BLACK .. C_LOGO3, converted at compile time */
#define RGB(r, g, b) gfx_RGBTo1555(r, g, b)
static const uint16_t baseRGB[C_FILL] = {
    RGB(0, 0, 0), RGB(255, 255, 255), RGB(28, 30, 40), RGB(150, 155, 170),     /* black white panel dim */
    RGB(255, 220, 60), RGB(255, 60, 50), RGB(80, 220, 90), RGB(110, 110, 120), /* yellow red green grey */
    RGB(10, 10, 16), RGB(255, 150, 30), RGB(80, 220, 255), RGB(52, 40, 34),    /* void orange cyan rail */
    RGB(40, 44, 58), RGB(24, 110, 220), RGB(240, 130, 40), RGB(20, 70, 36),    /* bar bg, bar, bar out, dk green */
    RGB(22, 42, 92), RGB(40, 72, 124), RGB(150, 190, 110), RGB(186, 176, 116), /* ocean lake plains highland */
    RGB(226, 226, 216), RGB(96, 214, 40), RGB(120, 196, 50), RGB(150, 220, 90), /* mountain, fallout x3 */
    RGB(70, 8, 8), RGB(255, 140, 110), RGB(235, 45, 45), RGB(180, 26, 26),     /* logo: edge, light, red, dark */
    RGB(14, 14, 22), RGB(40, 46, 64), RGB(0, 0, 0), RGB(0, 0, 0),              /* shadow, key cap */
};

void r_palette(void)
{
    uint16_t i;
    memcpy(gfx_palette, baseRGB, sizeof baseRGB);
    for (i = 0; i < 256; i++) {
        uint8_t b = (uint8_t)i, t = (b >> 5) & 3, c;
        if (!t)
            c = C_OCEAN;
        else if (b & T_OWNER)
            c = C_FILL + (b & T_OWNER);
        else
            c = (uint8_t)(((b & T_FALL) ? C_FPLAINS : C_PLAINS) + t - 1);
        colLUT[i] = c;
    }
    build_masks();
}

void r_player_colors(void)
{
    uint8_t p, n = 0;
    for (p = 1; p < MAXP; p++) {
        uint8_t r, g, b;
        if (p == HUMAN) {
            r = 235; g = 45; b = 45;
        } else if (g_pl[p].type == PT_TRIBE) {
            r = g = b = (uint8_t)(100 + (p * 37) % 80);
        } else {
            const uint8_t *c = nationRGB[n % 20];
            n++;
            r = c[0]; g = c[1]; b = c[2];
        }
        pal(C_FILL + p, r, g, b);
        pal(C_BORDER + p, (uint8_t)(r * 5 / 9), (uint8_t)(g * 5 / 9), (uint8_t)(b * 5 / 9));
    }
}

/* ------------------------------------------------------------- tiles --- */
#define DIFF(n, b) (((n) & T_TERR) && (((n) ^ (b)) & T_OWNER))

static int negW;

static void set_zoom_info(void)
{
    negW = -(int)g_W;
    zsh = zoom == 8 ? 3 : (zoom == 4 ? 2 : (zoom == 2 ? 1 : 0));
    railLo = zoom >> 2;
    railN = zoom >> 1;
}


/* colour of a map tile: terrain or owner, darker on borders */
#define TILE_COLOR(mp, c)                                                             \
    do {                                                                              \
        uint8_t b_ = *(mp);                                                           \
        c = colLUT[b_];                                                               \
        if (c >= C_FILL && (DIFF((mp)[1], b_) || DIFF((mp)[-1], b_) ||                 \
                            DIFF((mp)[g_W], b_) || DIFF((mp)[negW], b_)))             \
            c += 32;                                                                  \
    } while (0)

/* Repaint the map inside a screen rectangle (inclusive, viewport coords).
 * Walks each pixel row tile span by tile span: one colour lookup per tile. */
static void paint_rect(int x0, int y0, int x1, int y1)
{
    int y, w, tc0, my;
    uint8_t *rowp, xoff0, yoff;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= SCR_W) x1 = SCR_W - 1;
    if (y1 >= vh) y1 = vh - 1;
    if (x0 > x1 || y0 > y1) return;
    w = x1 - x0 + 1;
    rowp = VRAM + (unsigned)(vy0 + y0) * SCR_W + x0;
    tc0 = camx + (x0 >> zsh);
    xoff0 = (uint8_t)(x0 & (zoom - 1));
    my = camy + (y0 >> zsh);
    yoff = (uint8_t)(y0 & (zoom - 1));
    for (y = y0; y <= y1; y++, rowp += SCR_W) {
        if (my < 0 || my >= (int)g_H)
            memset(rowp, C_VOID, w);
        else {
            bool railRow = zoom > 2 && (uint8_t)(yoff - railLo) < railN;
            tile_t rowT = g_rowStart[my];
            const uint8_t *mp = g_map + rowT + tc0;
            uint8_t *d = rowp;
            int mx = tc0, left = w;
            uint8_t span = (uint8_t)(zoom - xoff0), inPos = xoff0;
            while (left > 0) {
                uint8_t c;
                bool rail = false;
                if (span > left) span = (uint8_t)left;
                if (mx < 0 || mx >= (int)g_W)
                    c = C_VOID;
                else {
                    TILE_COLOR(mp, c);
                    if (railOn && bitget(g_railbits, (tile_t)(rowT + mx))) {
                        if (zoom <= 2)
                            c = C_RAIL;
                        else
                            rail = railRow;
                    }
                }
                if (span == 1)
                    *d = c;
                else {
                    memset(d, c, span);
                    if (rail) {
                        /* rail mark: pixels railLo .. railLo+railN-1 of the tile */
                        uint8_t k;
                        for (k = 0; k < span; k++)
                            if ((uint8_t)(inPos + k - railLo) < railN) d[k] = C_RAIL;
                    }
                }
                d += span;
                left -= span;
                mp++;
                mx++;
                inPos = 0;
                span = zoom;
            }
        }
        if (++yoff == zoom) {
            yoff = 0;
            my++;
        }
    }
}

void ui_dirty(tile_t t)
{
    if (dqn >= DQ) {
        fullReq = true;
        return;
    }
    dq[(dqh + dqn) & (DQ - 1)] = t;
    dqn++;
}

void ui_dirty_unit(tile_t t)
{
    if (sqn >= SQ) {
        fullReq = true;
        return;
    }
    sq[sqn++] = t;
}

/* ----------------------------------------------------------- sprites --- */
__attribute__((noinline)) static void px(int x, int y, uint8_t c)
{
    if ((unsigned)x < SCR_W && (unsigned)y < vh) VRAM[(unsigned)(y + vy0) * SCR_W + x] = c;
}

__attribute__((noinline)) static void fill(int x0, int y0, int w, int h, uint8_t c)
{
    uint8_t *p;
    if (x0 < 0) {
        w += x0;
        x0 = 0;
    }
    if (y0 < 0) {
        h += y0;
        y0 = 0;
    }
    if (x0 + w > SCR_W) w = SCR_W - x0;
    if (y0 + h > vh) h = vh - y0;
    if (w <= 0 || h <= 0) return;
    p = VRAM + (unsigned)(y0 + vy0) * SCR_W + x0;
    while (h--) {
        memset(p, c, w);
        p += SCR_W;
    }
}

/* square outline, side n */
static void outline(int x, int y, int n, uint8_t c)
{
    fill(x, y, n, 1, c);
    fill(x, y + n - 1, n, 1, c);
    fill(x, y, 1, n, c);
    fill(x + n - 1, y, 1, n, c);
}

/* 7x7 structure glyphs, one byte per row, bit 6 = left */
static const uint8_t structGlyph[7][7] = {
    {0, 0, 0, 0, 0, 0, 0},
    {0x03, 0x33, 0x3B, 0x3F, 0x7F, 0x55, 0x7F}, /* city: skyline */
    {0x7F, 0x7F, 0x7F, 0x3E, 0x3E, 0x1C, 0x08}, /* defense post: shield */
    {0x08, 0x1C, 0x08, 0x08, 0x49, 0x2A, 0x1C}, /* port: anchor */
    {0x40, 0x40, 0x49, 0x5B, 0x7F, 0x55, 0x7F}, /* factory: chimney + saw roof */
    {0x08, 0x1C, 0x1C, 0x1C, 0x3E, 0x5D, 0x49}, /* silo: rocket */
    {0x41, 0x22, 0x14, 0x08, 0x1C, 0x3E, 0x7F}, /* SAM: radar */
};

/* ship glyphs, 7x7, one byte per row, bit 6 = left: a transport boat is an
 * "x", a trade ship a dot, a warship a "+" */
static const uint8_t shipGlyph[3][7] = {
    {0x00, 0x22, 0x36, 0x1C, 0x36, 0x22, 0x00}, /* transport boat */
    {0x00, 0x00, 0x1C, 0x1C, 0x1C, 0x00, 0x00}, /* trade ship */
    {0x1C, 0x1C, 0x7F, 0x7F, 0x7F, 0x1C, 0x1C}, /* warship */
};

/* pre-built masks: 0 = transparent, 1 = black outline, 2 = body colour;
 * a glyph pixel (x, y) is mask pixel (x + 1, y + 1), centre (4, 4) */
#define SM 9
static uint8_t shipMask[3][SM][SM];
/* structure badges: 0 = black frame, 1 = background, 2 = glyph */
static uint8_t structMask[7][9][9];

static void build_masks(void)
{
    uint8_t k, x, y;
    memset(shipMask, 0, sizeof shipMask);
    for (k = 0; k < 3; k++) {
        for (y = 0; y < 7; y++)
            for (x = 0; x < 7; x++)
                if (shipGlyph[k][y] & (0x40 >> x)) {
                    shipMask[k][y + 1][x] = shipMask[k][y + 1][x + 2] = 1;
                    shipMask[k][y][x + 1] = shipMask[k][y + 2][x + 1] = 1;
                }
        for (y = 0; y < 7; y++)
            for (x = 0; x < 7; x++)
                if (shipGlyph[k][y] & (0x40 >> x)) shipMask[k][y + 1][x + 1] = 2;
    }
    for (k = 1; k < 7; k++)
        for (y = 0; y < 9; y++)
            for (x = 0; x < 9; x++) {
                uint8_t v = 0;
                if (x && y && x < 8 && y < 8) v = (structGlyph[k][y - 1] & (0x40 >> (x - 1))) ? 2 : 1;
                structMask[k][y][x] = v;
            }
}

/* draw a mask with its top-left at (x0, y0); colours indexed by mask value */
static void blit(const uint8_t *m, uint8_t w, uint8_t h, int x0, int y0, const uint8_t *cols)
{
    uint8_t x, y;
    if (x0 >= 0 && y0 >= 0 && x0 + w <= SCR_W && y0 + h <= vh) {
        uint8_t *p = VRAM + (unsigned)(y0 + vy0) * SCR_W + x0;
        for (y = 0; y < h; y++, p += SCR_W)
            for (x = 0; x < w; x++, m++)
                if (*m || cols[0] != 0xFF) p[x] = cols[*m];
        return;
    }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++, m++)
            if (*m || cols[0] != 0xFF) px(x0 + x, y0 + y, cols[*m]);
}

/* screen centre of tile t */
FORCE_INLINE void scr_xy(tile_t t, int *x, int *y)
{
    int ty_ = ty(t);
    *x = (((int)(t - g_rowStart[ty_]) - camx) << zsh) + (zoom >> 1);
    *y = ((ty_ - camy) << zsh) + (zoom >> 1);
}

static void draw_struct(const unit_t *u, int cx, int cy)
{
    uint8_t cols[3];
    cols[0] = C_BLACK;
    cols[1] = C_BORDER + u->owner;
    cols[2] = BUILT(u) ? C_WHITE : C_GREY;
    blit(&structMask[u->type][0][0], 9, 9, cx - 4, cy - 4, cols);
    if ((u->aux & 1) && BUILT(u)) {
        /* train station: yellow corners */
        px(cx - 4, cy - 4, C_YELLOW);
        px(cx + 4, cy - 4, C_YELLOW);
    }
    if (u->level > 1) {
        uint8_t n = u->level, i;
        int x0 = cx - n + 1;
        fill(x0 - 1, cy + 5, 2 * n + 1, 3, C_BLACK);
        for (i = 0; i < n; i++) px(x0 + 2 * i, cy + 6, C_YELLOW);
    }
}

static void draw_ship(const unit_t *u, int cx, int cy)
{
    uint8_t cols[3];
    cols[0] = 0xFF; /* transparent */
    cols[1] = C_BLACK;
    cols[2] = C_FILL + u->owner;
    blit(&shipMask[u->type - U_BOAT][0][0], SM, SM, cx - 4, cy - 4, cols);
    if (u->type == U_TRADE && u->owner != u->orig) px(cx, cy, C_BLACK); /* captured */
    if (u->type == U_WARSHIP) {
        uint8_t max = (uint8_t)WS_MAXHP(u), w = (uint8_t)((u->hp * 9u + max - 1) / max), x;
        if (u->hp < max) {
            fill(cx - 5, cy - 7, 11, 3, C_BLACK);
            fill(cx - 4, cy - 6, w, 1, u->hp * 2 > max ? C_GREEN : C_RED);
        }
        for (x = 0; x < u->level; x++) px(cx - 1 + x, cy, C_YELLOW); /* veterancy */
    }
}

static void sprite(const unit_t *u, int cx, int cy)
{
    if (IS_STRUCT(u->type))
        draw_struct(u, cx, cy);
    else if (IS_SHIP(u->type))
        draw_ship(u, cx, cy);
    else if (u->type == U_TRAIN) {
        fill(cx - 2, cy - 2, 5, 5, C_BLACK);
        fill(cx - 1, cy - 1, 3, 3, C_FILL + u->owner);
    } else if (u->type == U_SHELL)
        fill(cx, cy, 2, 2, C_YELLOW);
    else if (IS_NUKE(u->type)) {
        fill(cx - 1, cy - 2, 3, 5, C_RED);
        fill(cx - 2, cy - 1, 5, 3, C_RED);
        px(cx, cy, u->type == U_MIRV ? C_YELLOW : C_WHITE);
    }
}

/* midpoint circle with per-pixel clipping; colour 0xFF repaints the map
 * under it instead */
#define UNDER 0xFF
static void ring(int cx, int cy, int r, uint8_t c)
{
    int x = r, y = 0, e = 1 - r;
    uint8_t k;
    while (x >= y) {
        for (k = 0; k < 8; k++) {
            int a = (k & 4) ? y : x, b = (k & 4) ? x : y;
            int X = cx + ((k & 1) ? -a : a), Y = cy + ((k & 2) ? -b : b);
            if (c == UNDER)
                paint_rect(X, Y, X, Y);
            else
                px(X, Y, c);
        }
        y++;
        if (e < 0)
            e += 2 * y + 1;
        else {
            x--;
            e += 2 * (y - x) + 1;
        }
    }
}

static bool on_screen(int cx, int cy) { return cx > -12 && cx < SCR_W + 12 && cy > -12 && cy < vh + 12; }

/* ---------------------------------------------------- dirty-cell grid --- */
/* The viewport is split into 16x16 pixel cells. A cell is marked when the
 * map under it is repainted (1) or a sprite is drawn in it (2); sprites and
 * the cursor are only redrawn where something changed. */
#define CSH 4
#define CW (SCR_W >> CSH)
#define CH 15
static uint8_t dcell[CH][CW];

static bool dirtyAny; /* some cell is marked this frame */

static void mark(int x0, int y0, int x1, int y1)
{
    int cx, cy;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= SCR_W) x1 = SCR_W - 1;
    if (y1 >= vh) y1 = vh - 1;
    if (x0 > x1 || y0 > y1) return;
    dirtyAny = true;
    for (cy = y0 >> CSH; cy <= y1 >> CSH; cy++)
        for (cx = x0 >> CSH; cx <= x1 >> CSH; cx++) dcell[cy][cx] = 1;
}

static bool touched(int x0, int y0, int x1, int y1)
{
    int cx, cy;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= SCR_W) x1 = SCR_W - 1;
    if (y1 >= vh) y1 = vh - 1;
    if (x0 > x1 || y0 > y1) return false;
    for (cy = y0 >> CSH; cy <= y1 >> CSH; cy++)
        for (cx = x0 >> CSH; cx <= x1 >> CSH; cx++)
            if (dcell[cy][cx]) return true;
    return false;
}

#ifdef CF_PERF
uint8_t g_perfNb, g_perfNm;
#endif

static uint8_t drawnHi; /* every sprite on screen came from a slot below this */
static int16_t lastSel = -1; /* selection box drawn last frame */

/* screen extent of a moving sprite around its centre: left, top, right, bottom */
static const int8_t spriteBox[U_TYPES][4] = {
    {0, 0, 0, 0},     {0, 0, 0, 0},     {0, 0, 0, 0},     {0, 0, 0, 0},
    {0, 0, 0, 0},     {0, 0, 0, 0},     {0, 0, 0, 0},
    {-3, -3, 3, 3},   /* boat */
    {-2, -2, 2, 2},   /* trade ship */
    {-5, -7, 5, 4},   /* warship (health bar above) */
    {-2, -2, 2, 2},   {-2, -2, 2, 2},   {-2, -2, 2, 2}, /* missiles */
    {0, 0, 1, 1},     /* shell */
    {-2, -2, 2, 2},   /* train */
    {0, 0, 0, 0},
};

static void sprite_rect(uint8_t type, bool sel, int cx, int cy, int *x0, int *y0, int *x1, int *y1)
{
    if (sel) {
        *x0 = cx - 8;
        *y0 = cy - 8;
        *x1 = cx + 8;
        *y1 = cy + 8;
        return;
    }
    *x0 = cx + spriteBox[type][0];
    *y0 = cy + spriteBox[type][1];
    *x1 = cx + spriteBox[type][2];
    *y1 = cy + spriteBox[type][3];
}

/* tiles already repainted this frame (direct-mapped, by low bits) */
static tile_t paintedT[64];

/* repaint one map tile (x, y) unless it was already repainted this frame */
static void repaint_tile(int x, int y, tile_t t)
{
    int sx, sy;
    u16b_t u;
    uint8_t slot;
    u.v = t;
    slot = u.b[0] & 63;
    if (paintedT[slot] == t) return;
    paintedT[slot] = t;
    sx = (x - camx) << zsh;
    sy = (y - camy) << zsh;
    if (sx < 0 || sx >= SCR_W || sy < 0 || sy >= vh) return;
    if (sy + zoom > vh) {
        /* cut by the bottom of the view */
        paint_rect(sx, sy, sx + zoom - 1, sy + zoom - 1);
        return;
    }
    {
        /* a whole tile: one colour, zoom x zoom pixels, rail mark in the middle */
        uint8_t c, k, *d = VRAM + (unsigned)(sy + vy0) * SCR_W + sx;
        bool rail = false;
        const uint8_t *mp = g_map + t;
        if (x < 0 || y < 0 || x >= (int)g_W || y >= (int)g_H)
            c = C_VOID;
        else {
            TILE_COLOR(mp, c);
            if (railOn && bitget(g_railbits, t)) {
                if (zoom <= 2)
                    c = C_RAIL;
                else
                    rail = true;
            }
        }
        switch (zoom) {
        case 1:
            *d = c;
            break;
        case 2:
            d[0] = d[1] = d[SCR_W] = d[SCR_W + 1] = c;
            break;
        default:
            for (k = 0; k < zoom; k++, d += SCR_W) {
                memset(d, c, zoom);
                if (rail && (uint8_t)(k - railLo) < railN) memset(d + railLo, C_RAIL, railN);
            }
        }
    }
}

/* Draw sprites: buildings first, then everything that moves, each in slot
 * order. A sprite is drawn when it moved or changed, or when the map or an
 * earlier sprite was painted where it stands (all = everything). */
static void overlays(bool all)
{
    uint8_t i, hi = g_unHi;
    const unit_t *u;
    PERF_START();
#ifdef CF_PERF
    g_perfNb = g_perfNm = 0;
#endif
    if (hi > drawnHi) drawnHi = hi;
    for (i = 0, u = g_un; i < hi; i++, u++) {
        int cx, cy;
        if (!IS_STRUCT(u->type)) continue;
        drawnAt[i] = NO_TILE;
        drawnType[i] = u->type;
        if (!all && !dirtyAny) continue; /* nothing painted over: still on screen */
        scr_xy(u->pos, &cx, &cy);
        if (!on_screen(cx, cy) || !(all || touched(cx - 9, cy - 4, cx + 9, cy + 7))) continue;
#ifdef CF_PERF
        g_perfNb++;
#endif
        sprite(u, cx, cy);
        mark(cx - 9, cy - 4, cx + 9, cy + 7);
    }
    PERF_ADD(4);
    for (i = 0, u = g_un; i < hi; i++, u++) {
        int cx, cy;
        uint8_t t = u->type;
        bool changed;
        if (t == U_NONE) {
            drawnType[i] = 0; /* moving sprites were erased already, buildings via sq */
            continue;
        }
        if (IS_STRUCT(t)) continue;
        if (zoom == 1 && (t == U_TRADE || t == U_TRAIN)) {
            /* zoomed all the way out: too small to matter, too many to draw */
            drawnType[i] = 0;
            continue;
        }
        changed = drawnType[i] != t || drawnAt[i] != u->pos;
        drawnAt[i] = u->pos;
        drawnType[i] = t;
        /* what else forces a repaint: warship health, explosion size */
        drawnLevel[i] = t == U_WARSHIP ? u->hp : u->level;
        if (!all && !changed && !dirtyAny && i != r_sel && t != U_BOOM) continue;
        scr_xy(u->pos, &cx, &cy);
        if (t == U_BOOM) {
            int r = u->level * zoom;
            ring(cx, cy, r, (u->timer & 1) ? C_YELLOW : C_RED);
            mark(cx - r, cy - r, cx + r, cy + r);
            continue;
        }
        if (!on_screen(cx, cy)) continue;
        {
            int bx0, by0, bx1, by1;
            sprite_rect(t, i == r_sel, cx, cy, &bx0, &by0, &bx1, &by1);
            if (!(all || changed || i == r_sel || touched(bx0, by0, bx1, by1))) continue;
#ifdef CF_PERF
            g_perfNm++;
#endif
            sprite(u, cx, cy);
            mark(bx0, by0, bx1, by1);
        }
        if (i == r_sel) {
            outline(cx - 8, cy - 8, 17, (clock() & (CLOCKS_PER_SEC / 4)) ? C_YELLOW : C_WHITE);
        }
    }
    PERF_ADD(5);
}

/* cursor bounding box on screen (viewport coords) */
static void cursor_box(int cx, int cy, int r, int *x0, int *y0, int *x1, int *y1)
{
    int m = zoom >= 4 ? 1 : 6, c = zoom >> 1;
    *x0 = cx - m;
    *y0 = cy - m;
    *x1 = cx + (zoom >= 4 ? zoom : c + 6);
    *y1 = cy + (zoom >= 4 ? zoom : c + 6);
    if (zoom < 4) {
        *x0 = cx + c - 6;
        *y0 = cy + c - 6;
    }
    if (r) {
        if (cx + c - r < *x0) *x0 = cx + c - r;
        if (cy + c - r < *y0) *y0 = cy + c - r;
        if (cx + c + r > *x1) *x1 = cx + c + r;
        if (cy + c + r > *y1) *y1 = cy + c + r;
    }
}

static void cursor(void)
{
    int cx = ((int)curx - camx) << zsh, cy = ((int)cury - camy) << zsh, k;
    if (zoom >= 4) {
        outline(cx, cy, zoom, C_WHITE);
        outline(cx - 1, cy - 1, zoom + 2, C_BLACK);
    } else {
        int mx = cx + (zoom >> 1), my = cy + (zoom >> 1);
        for (k = 2; k <= 6; k++) {
            uint8_t c = k == 6 ? C_BLACK : C_WHITE;
            px(mx + k, my, c);
            px(mx - k, my, c);
            px(mx, my + k, c);
            px(mx, my - k, c);
        }
        px(mx, my, C_WHITE);
    }
    curSX = cx;
    curSY = cy;
    curRing = nukeRing * zoom;
    if (curRing) ring(cx + (zoom >> 1), cy + (zoom >> 1), curRing, C_RED);
}

static void erase_cursor(void)
{
    if (curSX == -100) return;
    if (zoom >= 4)
        paint_rect(curSX - 1, curSY - 1, curSX + zoom, curSY + zoom);
    else {
        int mx = curSX + (zoom >> 1), my = curSY + (zoom >> 1);
        paint_rect(mx - 6, my, mx + 6, my);
        paint_rect(mx, my - 6, mx, my - 1);
        paint_rect(mx, my + 1, mx, my + 6);
    }
    if (curRing) ring(curSX + (zoom >> 1), curSY + (zoom >> 1), curRing, UNDER);
    curSX = -100;
}

/* -------------------------------------------------------------- frame --- */
void r_reset(void)
{
    dqn = dqh = 0;
    sqn = 0;
    fullReq = true;
    memset(drawnAt, 0xFF, sizeof drawnAt);
    memset(drawnType, 0, sizeof drawnType);
    drawnHi = 0;
    curSX = -100;
    nukeRing = 0;
    r_sel = -1;
}

void r_request_full(void) { fullReq = true; }

void r_clamp_cam(void)
{
    int tw = SCR_W / zoom, th = vh / zoom;
    set_zoom_info();
    if ((int)g_W <= tw)
        camx = -((tw - (int)g_W) / 2);
    else {
        if (camx < 0) camx = 0;
        if (camx > (int)g_W - tw) camx = (int)g_W - tw;
    }
    if ((int)g_H <= th)
        camy = -((th - (int)g_H) / 2);
    else {
        if (camy < 0) camy = 0;
        if (camy > (int)g_H - th) camy = (int)g_H - th;
    }
}

void r_center(uint16_t x, uint16_t y)
{
    set_zoom_info();
    camx = (int)x - SCR_W / zoom / 2;
    camy = (int)y - vh / zoom / 2;
    r_clamp_cam();
    fullReq = true;
}

/* keep the cursor inside the view; returns true if the camera moved */
bool r_follow_cursor(void)
{
    int tw = SCR_W / zoom, th = vh / zoom, ox = camx, oy = camy;
    int m = zoom >= 4 ? 2 : 6;
    if ((int)curx < camx + m) camx = (int)curx - tw / 3;
    if ((int)curx >= camx + tw - m) camx = (int)curx - tw + tw / 3;
    if ((int)cury < camy + m) camy = (int)cury - th / 3;
    if ((int)cury >= camy + th - m) camy = (int)cury - th + th / 3;
    r_clamp_cam();
    return ox != camx || oy != camy; /* r_frame slides the picture */
}

void r_full(void)
{
    int tw, th, rx, ry, xin0, xin1;
    uint8_t *dst = VRAM + (unsigned)vy0 * SCR_W, k;
    set_zoom_info();
    tw = SCR_W >> zsh;
    th = vh >> zsh;
    railOn = rail_any();
    fullReq = false;
    dqn = 0;
    sqn = 0;
    /* screen columns of tiles that exist */
    xin0 = camx < 0 ? -camx : 0;
    xin1 = (int)g_W - camx;
    if (xin1 > tw) xin1 = tw;
    for (ry = 0; ry < th; ry++) {
        int ty_ = camy + ry;
        bool anyRail = false;
        if (ty_ < 0 || ty_ >= (int)g_H || xin0 >= xin1)
            memset(line, C_VOID, SCR_W);
        else {
            tile_t row = g_rowStart[ty_];
            const uint8_t *mp = g_map + row + camx + xin0;
            uint8_t *lp = line;
            if (xin0) {
                memset(line, C_VOID, (unsigned)xin0 << zsh);
                lp += (unsigned)xin0 << zsh;
            }
            for (rx = xin0; rx < xin1; rx++, mp++) {
                uint8_t c;
                TILE_COLOR(mp, c);
                if (railOn && bitget(g_railbits, (tile_t)(mp - g_map))) {
                    if (zoom <= 2)
                        c = C_RAIL;
                    else
                        anyRail = true;
                }
                switch (zoom) {
                case 8: *lp++ = c; *lp++ = c; *lp++ = c; *lp++ = c; /* fallthrough */
                case 4: *lp++ = c; *lp++ = c; /* fallthrough */
                case 2: *lp++ = c; /* fallthrough */
                default: *lp++ = c;
                }
            }
            if (xin1 < tw) memset(lp, C_VOID, (unsigned)(tw - xin1) << zsh);
            if (anyRail) {
                /* same row with the rail marks in the middle of rail tiles */
                memcpy(rline, line, SCR_W);
                for (rx = xin0; rx < xin1; rx++)
                    if (bitget(g_railbits, (tile_t)(row + camx + rx)))
                        memset(rline + (rx << zsh) + railLo, C_RAIL, railN);
            }
        }
        for (k = 0; k < zoom; k++) {
            memcpy(dst, anyRail && (uint8_t)(k - railLo) < railN ? rline : line, SCR_W);
            dst += SCR_W;
        }
    }
    /* the last row of tiles, cut by the bottom of the view */
    if ((th << zsh) < vh) paint_rect(0, th << zsh, SCR_W - 1, vh - 1);
    drawnCamX = camx;
    drawnCamY = camy;
    memset(dcell, 1, sizeof dcell);
    dirtyAny = true;
    overlays(true);
    cursor();
}

/* The camera moved by less than a screen: slide the picture in video memory
 * by the same amount (whole tiles), then paint the strips that came into
 * view and mark them so sprites cut by the old edge are drawn again.
 * Returns false when a full redraw is cheaper. */
static bool slide_view(void)
{
    int px = (camx - drawnCamX) * zoom, py = (camy - drawnCamY) * zoom;
    int apx = px < 0 ? -px : px, apy = py < 0 ? -py : py, w = SCR_W - apx, y;
    uint8_t *base = VRAM + (unsigned)vy0 * SCR_W;
    unsigned dx = px < 0 ? (unsigned)-px : 0, sx = px > 0 ? (unsigned)px : 0;
    if (apx >= SCR_W * 3 / 4 || apy >= vh * 3 / 4) return false;
    /* new pixel (x, y) = old pixel (x + px, y + py): copy rows away from
     * the side they move to, so no row is overwritten before it is read */
    if (py >= 0)
        for (y = 0; y < vh - py; y++)
            memmove(base + (unsigned)y * SCR_W + dx, base + (unsigned)(y + py) * SCR_W + sx, w);
    else
        for (y = vh - 1; y >= -py; y--)
            memmove(base + (unsigned)y * SCR_W + dx, base + (unsigned)(y + py) * SCR_W + sx, w);
    if (px > 0) {
        paint_rect(SCR_W - px, 0, SCR_W - 1, vh - 1);
        mark(SCR_W - px, 0, SCR_W - 1, vh - 1);
    } else if (px < 0) {
        paint_rect(0, 0, -px - 1, vh - 1);
        mark(0, 0, -px - 1, vh - 1);
    }
    if (py > 0) {
        paint_rect(0, vh - py, SCR_W - 1, vh - 1);
        mark(0, vh - py, SCR_W - 1, vh - 1);
    } else if (py < 0) {
        paint_rect(0, 0, SCR_W - 1, -py - 1);
        mark(0, 0, SCR_W - 1, -py - 1);
    }
    /* the cursor's pixels moved with the picture */
    if (curSX != -100) {
        curSX -= px;
        curSY -= py;
    }
    drawnCamX = camx;
    drawnCamY = camy;
    return true;
}

void r_frame(void)
{
    uint8_t i, hi;
    uint16_t n;
    const unit_t *u;
    int ncx, ncy, x0, y0, x1, y1;
    bool curMoved;
    PERF_START();
    railOn = rail_any();
    if (fullReq) {
        r_full();
        PERF_ADD(9);
        return;
    }
    memset(dcell, 0, sizeof dcell);
    dirtyAny = false;
    memset(paintedT, 0xFF, sizeof paintedT);
    if ((camx != drawnCamX || camy != drawnCamY) && !slide_view()) {
        r_full();
        PERF_ADD(9);
        return;
    }
    /* map under sprites that moved, changed or vanished */
    hi = drawnHi > g_unHi ? drawnHi : g_unHi;
    for (i = 0, u = g_un; i < hi; i++, u++) {
        uint8_t dt = drawnType[i];
        if (!dt || IS_STRUCT(dt)) continue;
        if (dt == U_BOOM) {
            if (u->type != U_BOOM) {
                /* explosion over: repaint its whole area once */
                int cx, cy, r = drawnLevel[i] * zoom + 1;
                scr_xy(drawnAt[i], &cx, &cy);
                paint_rect(cx - r, cy - r, cx + r, cy + r);
                mark(cx - r, cy - r, cx + r, cy + r);
                drawnType[i] = 0;
            }
            continue;
        }
        if (u->type != dt || u->pos != drawnAt[i] || i == r_sel || i == lastSel ||
            (dt == U_WARSHIP && u->hp != drawnLevel[i])) {
            int cx, cy;
            scr_xy(drawnAt[i], &cx, &cy);
            if (on_screen(cx, cy)) {
                int bx0, by0, bx1, by1;
                sprite_rect(dt, i == r_sel || i == lastSel, cx, cy, &bx0, &by0, &bx1, &by1);
                paint_rect(bx0, by0, bx1, by1);
                mark(bx0, by0, bx1, by1);
            }
            if (u->type == U_NONE) drawnType[i] = 0;
        }
    }
    if (g_unHi <= drawnHi) {
        while (drawnHi && !drawnType[drawnHi - 1]) drawnHi--;
    }
    lastSel = r_sel;
    PERF_ADD(6);
    /* the cursor: only when it moved or its nuke ring changed */
    ncx = ((int)curx - camx) << zsh;
    ncy = ((int)cury - camy) << zsh;
    curMoved = ncx != curSX || ncy != curSY || nukeRing * zoom != curRing;
    if (curMoved && curSX != -100) {
        cursor_box(curSX, curSY, curRing, &x0, &y0, &x1, &y1);
        erase_cursor();
        mark(x0, y0, x1, y1);
    }
    PERF_ADD(8);
    for (i = 0; i < sqn; i++) {
        int cx, cy;
        scr_xy(sq[i], &cx, &cy);
        paint_rect(cx - 9, cy - 4, cx + 9, cy + 7);
        mark(cx - 9, cy - 4, cx + 9, cy + 7);
    }
    sqn = 0;
    for (n = dqn; n; n--) {
        tile_t t = dq[dqh];
        uint16_t y = ty(t);
        int x = (int)(t - g_rowStart[y]), sx = (x - camx) << zsh, sy = ((int)y - camy) << zsh;
        dqh = (dqh + 1) & (DQ - 1);
        if (sx < -zoom || sx > SCR_W || sy < -zoom || sy > vh) continue;
        /* the tile and its 4 neighbours (their border shading may change) */
        repaint_tile(x, (int)y, t);
        repaint_tile(x - 1, (int)y, (tile_t)(t - 1));
        repaint_tile(x + 1, (int)y, (tile_t)(t + 1));
        repaint_tile(x, (int)y - 1, (tile_t)(t - g_W));
        repaint_tile(x, (int)y + 1, (tile_t)(t + g_W));
        mark(sx - zoom, sy - zoom, sx + 2 * zoom - 1, sy + 2 * zoom - 1);
    }
    dqn = 0;
    PERF_ADD(7);
    if (fullReq) {
        r_full();
        PERF_ADD(9);
        return;
    }
    overlays(false);
    PERF_SKIP();
    cursor_box(ncx, ncy, nukeRing * zoom, &x0, &y0, &x1, &y1);
    if (curMoved || touched(x0, y0, x1, y1)) cursor();
    PERF_ADD(8);
}

/* the home screen's backdrop: the map at one pixel per tile, centred in the
 * viewport, borders and all (the margins take the panel colour) */
void r_backdrop(void)
{
    uint16_t v = gfx_palette[C_VOID];
    zoom = 1;
    set_zoom_info();
    railOn = false;
    camx = 0;
    camy = 0;
    r_clamp_cam();
    if ((int)g_H > vh) camy = ((int)g_H - vh) / 2;
    gfx_palette[C_VOID] = gfx_palette[C_PANEL];
    paint_rect(0, 0, SCR_W - 1, vh - 1);
    gfx_palette[C_VOID] = v;
}

/* a unit's sprite at screen position (x, y), in the human's colours (help) */
void r_icon(uint8_t type, int x, int y)
{
    unit_t u;
    uint8_t oy = vy0, oh = vh;
    memset(&u, 0, sizeof u);
    u.type = type;
    u.owner = u.orig = HUMAN;
    u.hp = type == U_WARSHIP ? 100 : 1;
    u.level = type == U_WARSHIP ? 0 : 1;
    vy0 = 0;
    vh = 240;
    sprite(&u, x, y);
    vy0 = oy;
    vh = oh;
}

/* whole map, shrunk to fit the viewport (setup screen) */
void r_preview(void)
{
    uint8_t step = 1;
    int sx, sy, w, h, ox, oy;
    while ((int)g_W > SCR_W * step || (int)g_H > vh * step) step++;
    w = g_W / step;
    h = g_H / step;
    ox = (SCR_W - w) / 2;
    oy = (vh - h) / 2;
    memset(VRAM + (unsigned)vy0 * SCR_W, C_VOID, (unsigned)vh * SCR_W);
    for (sy = 0; sy < h; sy++) {
        uint8_t *dst = VRAM + (unsigned)(vy0 + oy + sy) * SCR_W + ox;
        tile_t row = g_rowStart[sy * step];
        for (sx = 0; sx < w; sx++) dst[sx] = colLUT[g_map[row + sx * step]];
    }
}
