/* CalcFront - calculator front end (graphics, input, menus) */
#ifndef UI_H
#define UI_H

#include <graphx.h>
#include "../sim/sim.h"

enum {
    C_BLACK, C_WHITE, C_PANEL, C_DIM, C_YELLOW, C_RED, C_GREEN, C_GREY, C_VOID,
    C_ORANGE, C_CYAN, C_RAIL, C_BARBG, C_BAR, C_BARATK, C_DKGREEN,
    C_OCEAN = 16, C_LAKE, C_PLAINS, C_HIGH, C_MOUNT, C_FPLAINS, C_FHIGH, C_FMOUNT,
    C_FILL = 32,   /* + player id */
    C_BORDER = 64  /* + player id */
};

#define SCR_W 320
#define TOP_H 20
#define MAP_Y0 TOP_H
#define MAP_H 190
#define INFO_Y 210
#define KEYS_Y 220

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
void hud_text(const char *s, int x, int y, uint8_t color);
void hud_center(const char *s, int y, uint8_t color);
void hud_top(void);
void hud_info(const char *s, uint8_t color);
void hud_keys(const char *const *top, const char *const *bottom);
void hud_panel(void);

#endif
