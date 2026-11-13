/* CalcFront - calculator front end (graphics, input, menus) */
#ifndef UI_H
#define UI_H

#include <graphx.h>
#include "../sim/sim.h"

enum {
    C_BLACK, C_WHITE, C_PANEL, C_DIM, C_YELLOW, C_RED, C_GREEN, C_GREY, C_VOID,
    C_ORANGE, C_CYAN, C_RAIL, C_BARBG, C_BAR, C_BARATK, C_DKGREEN,
    C_OCEAN = 16, C_LAKE, C_PLAINS, C_HIGH, C_MOUNT, C_FPLAINS, C_FHIGH, C_FMOUNT,
    C_LOGO0, C_LOGO1, C_LOGO2, C_LOGO3, C_SHADOW, C_KEYCAP, /* title and help screens */
    C_FILL = 32,   /* + player id */
    C_BORDER = 64  /* + player id */
};

#define SCR_W 320

/* screen layout: slim bars in normal mode, detailed ones in pro mode
 *   normal: status 0-11, map 12-217, info 218-227, keys 228-239
 *   pro:    status 0-19, map 20-209, info 210-219, keys 220-239 */
extern bool proMode;
extern uint8_t topH, infoY, keysY;
void ui_layout(void);

/* render.c */
extern int camx, camy;       /* top-left tile of the view (may be negative) */
extern uint8_t zoom;         /* pixels per tile: 1, 2, 4 or 8 */
extern uint16_t curx, cury;  /* cursor tile */
extern uint8_t vy0, vh;      /* viewport rows on screen */
extern uint8_t nukeRing;     /* radius (tiles) drawn around the cursor, 0 = off */
extern int16_t r_sel;        /* unit highlighted (selected warship), -1 = none */
void r_palette(void);
void r_player_colors(void);
void r_reset(void);
void r_full(void);
void r_frame(void);
void r_preview(void);
void r_backdrop(void);
void r_icon(uint8_t type, int x, int y);
void r_request_full(void);
void r_clamp_cam(void);
void r_center(uint16_t x, uint16_t y);
bool r_follow_cursor(void);

/* hud.c */
void fmt_num(char *dst, uint32_t v);
void fmt_troops(char *dst, uint32_t v);
char *s_cat(char *dst, const char *src);
char *s_num(char *dst, uint32_t v);
char *s_troops(char *dst, uint32_t v);
char *s_pct(char *dst, uint16_t p100, bool twoDecimals); /* 0.01% units */
char *s_time(char *dst, uint32_t seconds);               /* m:ss or h:mm:ss */
char *s_fmt(char *dst, const char *f, ...);              /* see hud.c */
uint32_t troops_out(uint8_t p);                          /* in attacks and boats */
void hud_text(const char *s, int x, int y, uint8_t color);
void hud_center(const char *s, int y, uint8_t color);
void hud_invalidate(void); /* the next hud_* calls redraw everything */
void hud_top(void);
void hud_info(const char *s, uint8_t color, const char *right);
void hud_keys(const char *const *top, const char *const *bottom, const uint8_t *colors);
void hud_panel(void);
void hud_logo(int y);                          /* the CALCFRONT logo, centred */
int hud_keycap(const char *s, int x, int y);   /* a key drawn as a cap; returns its width */
void hud_lines(const char *s, int x, int y, uint8_t color); /* text with \n line breaks */

#endif
