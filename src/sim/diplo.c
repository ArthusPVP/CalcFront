/* alliances and relations */
#include "sim.h"

#define ALLY_TICKS16 188 /* 5 minutes, in units of 16 ticks */

void rel_add(uint8_t p, uint8_t other, int8_t d)
{
    int16_t v;
    if (!p || !other || p == other) return;
    v = g_pl[p].rel[other] + d;
    if (v > 100) v = 100;
    if (v < -100) v = -100;
    g_pl[p].rel[other] = (int8_t)v;
}

static void make_allies(uint8_t a, uint8_t b)
{
    uint16_t until = (uint16_t)((g_tick >> 4) + ALLY_TICKS16);
    g_pl[a].allies |= BIT(b);
    g_pl[b].allies |= BIT(a);
    g_pl[a].allyUntil[b] = g_pl[b].allyUntil[a] = until;
    g_pl[a].reqFrom &= ~BIT(b);
    g_pl[b].reqFrom &= ~BIT(a);
    rel_add(a, b, 40);
    rel_add(b, a, 40);
    attack_retreat(a, b);
    attack_retreat(b, a);
}

void ally_request(uint8_t from, uint8_t to)
{
    if (from == to || !g_pl[from].alive || !g_pl[to].alive || ALLIED(from, to)) return;
    if (MASKBIT(g_pl[from].reqFrom, to)) {
        /* they already asked us: that is a yes */
        make_allies(from, to);
        if (from == HUMAN || to == HUMAN) ui_event(EV_ALLY_OK, from == HUMAN ? to : from, 0);
        return;
    }
    g_pl[to].reqFrom |= BIT(from);
    if (to == HUMAN) ui_event(EV_ALLY_REQ, from, 0);
}

void ally_answer(uint8_t from, uint8_t to, bool yes)
{
    /* `to` answers the request made by `from` */
    if (!MASKBIT(g_pl[to].reqFrom, from)) return;
    g_pl[to].reqFrom &= ~BIT(from);
    if (yes && g_pl[from].alive && g_pl[to].alive)
        make_allies(from, to);
    else
        rel_add(from, to, -5);
    if (from == HUMAN) ui_event(yes ? EV_ALLY_OK : EV_ALLY_NO, to, 0);
}

void ally_break(uint8_t a, uint8_t b)
{
    uint8_t i;
    if (!ALLIED(a, b)) return;
    g_pl[a].allies &= ~BIT(b);
    g_pl[b].allies &= ~BIT(a);
    g_pl[a].traitor = 300;
    rel_add(b, a, -100);
    /* everyone trusts a traitor less */
    for (i = 1; i <= g_nplayers; i++)
        if (i != a && i != b) rel_add(i, a, -20);
    if (b == HUMAN) ui_event(EV_BETRAYED, a, 0);
}

void diplo_tick(void)
{
    uint8_t a, b;
    uint16_t now = (uint16_t)(g_tick >> 4);
    for (a = 1; a <= g_nplayers; a++) {
        player_t *pa = &g_pl[a];
        if (!pa->alive || !pa->allies) continue;
        for (b = (uint8_t)(a + 1); b <= g_nplayers; b++) {
            if (!MASKBIT(pa->allies, b)) continue;
            if ((int16_t)(now - pa->allyUntil[b]) >= 0) {
                pa->allies &= ~BIT(b);
                g_pl[b].allies &= ~BIT(a);
                if (a == HUMAN || b == HUMAN) ui_event(EV_ALLY_END, a == HUMAN ? b : a, 0);
            }
        }
    }
}
