/* factories, train stations, railways and trains (OpenFront rules, simplified)
 *
 * A finished factory turns every city, port and factory within range
 * (110 original tiles) into a train station. Stations are linked by straight
 * railways over land to up to 4 of their nearest stations. Factories send
 * trains to a random reachable city or port; every city or port a train stops
 * at pays its owner, and the station owner too when it is someone else. */
#include "sim.h"

#define MAX_ADJ 4
#define TRAIN_CAP 16
#define NONE 0xFF

static uint8_t adj[MAX_UNITS][MAX_ADJ];
static bool dirty;
/* railway segments currently drawn, as (lower tile, higher tile) pairs, so a
 * rebuild only repaints the track that actually changed */
#define MAX_EDGES 256
static tile_t edgeA[2][MAX_EDGES], edgeB[2][MAX_EDGES];
static uint16_t nEdges[2];
static uint8_t cur;
static uint8_t bfsPrev[MAX_UNITS], bfsQ[MAX_UNITS];

void rail_mark_dirty(void) { dirty = true; }

void rail_reset(void)
{
    memset(adj, NONE, sizeof adj);
    dirty = false;
    nEdges[0] = nEdges[1] = 0;
}

bool is_station(const unit_t *u) { return IS_STATIONABLE(u->type) && BUILT(u) && (u->aux & 1); }

static uint8_t range_tiles(void) { return orig2tiles(110); }

static bool land_line(tile_t a, tile_t b)
{
    uint16_t n = line_len(a, b), k;
    for (k = 1; k < n; k++)
        if (!IS_LAND(g_map[line_point(a, b, k, n)])) return false;
    return true;
}

static void draw_line(tile_t a, tile_t b)
{
    uint16_t n = line_len(a, b), k;
    for (k = 1; k < n; k++) {
        tile_t t = line_point(a, b, k, n);
        bitset(g_railbits, t);
    }
}

static void dirty_line(tile_t a, tile_t b)
{
    uint16_t n = line_len(a, b), k;
    for (k = 1; k < n; k++) ui_dirty(line_point(a, b, k, n));
}

static bool has_edge(uint8_t set, tile_t a, tile_t b)
{
    uint16_t k;
    for (k = 0; k < nEdges[set]; k++)
        if (edgeA[set][k] == a && edgeB[set][k] == b) return true;
    return false;
}

static uint8_t degree(uint8_t i)
{
    uint8_t k, n = 0;
    for (k = 0; k < MAX_ADJ; k++)
        if (adj[i][k] != NONE) n++;
    return n;
}

static bool linked(uint8_t a, uint8_t b)
{
    uint8_t k;
    for (k = 0; k < MAX_ADJ; k++)
        if (adj[a][k] == b) return true;
    return false;
}

static void link(uint8_t a, uint8_t b)
{
    uint8_t k;
    for (k = 0; k < MAX_ADJ; k++)
        if (adj[a][k] == NONE) {
            adj[a][k] = b;
            break;
        }
    for (k = 0; k < MAX_ADJ; k++)
        if (adj[b][k] == NONE) {
            adj[b][k] = a;
            break;
        }
}

static void rebuild(void)
{
    uint8_t i, j, r = range_tiles(), rr = (uint8_t)(r + r / 3), nw = cur ^ 1;
    uint16_t k;
    dirty = false;
    memset(adj, NONE, sizeof adj);
    nEdges[nw] = 0;

    /* stations: near a finished factory */
    for (i = 0; i < g_unHi; i++) {
        unit_t *u = &g_un[i];
        if (!IS_STATIONABLE(u->type)) continue;
        u->aux &= (uint8_t)~1;
        if (!BUILT(u)) continue;
        for (j = 0; j < g_unHi; j++) {
            unit_t *f = &g_un[j];
            if (f->type == U_FACTORY && BUILT(f) && near(f->pos, u->pos, r)) {
                u->aux |= 1;
                break;
            }
        }
    }
    /* links: each station to its nearest stations, straight over land */
    for (i = 0; i < g_unHi; i++) {
        uint8_t blocked[6], nb = 0, k;
        if (!is_station(&g_un[i])) continue;
        while (degree(i) < MAX_ADJ && nb < sizeof blocked) {
            uint8_t best = NONE;
            uint16_t bd = 0xFFFF;
            for (j = 0; j < g_unHi; j++) {
                uint16_t d;
                if (j == i || !is_station(&g_un[j]) || linked(i, j) || degree(j) >= MAX_ADJ)
                    continue;
                for (k = 0; k < nb && blocked[k] != j; k++)
                    ;
                if (k < nb || !near(g_un[i].pos, g_un[j].pos, rr)) continue;
                d = mdist(g_un[i].pos, g_un[j].pos);
                if (d < bd) {
                    bd = d;
                    best = j;
                }
            }
            if (best == NONE) break;
            if (land_line(g_un[i].pos, g_un[best].pos)) {
                tile_t a = g_un[i].pos, b = g_un[best].pos;
                link(i, best);
                if (nEdges[nw] < MAX_EDGES) {
                    edgeA[nw][nEdges[nw]] = a < b ? a : b;
                    edgeB[nw][nEdges[nw]++] = a < b ? b : a;
                }
            } else
                blocked[nb++] = best; /* water in the way */
        }
    }
    /* redraw the bitmap, repaint only what changed */
    memset(g_railbits, 0, sizeof g_mem->railbits);
    for (k = 0; k < nEdges[nw]; k++) draw_line(edgeA[nw][k], edgeB[nw][k]);
    for (k = 0; k < nEdges[cur]; k++)
        if (!has_edge(nw, edgeA[cur][k], edgeB[cur][k])) dirty_line(edgeA[cur][k], edgeB[cur][k]);
    for (k = 0; k < nEdges[nw]; k++)
        if (!has_edge(cur, edgeA[nw][k], edgeB[nw][k])) dirty_line(edgeA[nw][k], edgeB[nw][k]);
    cur = nw;
}

/* breadth-first search over the station graph; returns the first hop from
 * `from` toward `to`, or NONE */
static uint8_t next_hop(uint8_t from, uint8_t to)
{
    uint8_t qh = 0, qt = 0, k, cur;
    memset(bfsPrev, NONE, sizeof bfsPrev);
    bfsPrev[from] = from;
    bfsQ[qt++] = from;
    while (qh < qt) {
        cur = bfsQ[qh++];
        if (cur == to) break;
        for (k = 0; k < MAX_ADJ; k++) {
            uint8_t n = adj[cur][k];
            if (n == NONE || bfsPrev[n] != NONE) continue;
            bfsPrev[n] = cur;
            bfsQ[qt++] = n;
        }
    }
    if (bfsPrev[to] == NONE) return NONE;
    cur = to;
    while (bfsPrev[cur] != from) cur = bfsPrev[cur];
    return cur;
}

void rail_tick(void)
{
    if (dirty && (g_tick & 7) == 3) rebuild();
}

static bool trade_stop(const unit_t *s, uint8_t owner)
{
    return (s->type == U_CITY || s->type == U_PORT) && (s->owner == owner || can_trade(owner, s->owner));
}

void factory_tick(uint8_t i)
{
    unit_t *f = &g_un[i], *t;
    uint8_t p = f->owner, qh = 0, qt = 0, k, cur, n = 0, pick, hop, l;
    uint16_t lv;
    uint32_t rate;

    if (!is_station(f) || (((uint8_t)g_tick + i) & 7)) return;
    if (f->timer) return; /* cooldown */
    if (g_nTrain >= TRAIN_CAP) return;

    /* OpenFront: rate = (factories + 20) * 165 / 21, sped up for small
     * economies; checked every 8 ticks instead of every tick */
    lv = levels_of(p, U_FACTORY);
    rate = ((uint32_t)(lv + 20) * 165 / 21) / 8;
    if (lv < 6) rate = rate * 256 / (256 + ((128u * ((6 - lv) * 256 / 5)) >> 8) / (lv ? lv : 1));
    rate = rate * TRAIN_CAP / (TRAIN_CAP - g_nTrain);
    if (!rate) rate = 1;
    for (l = 0; l < f->level; l++)
        if (rnd_n((uint16_t)rate) == 0) break;
    if (l == f->level) return;

    /* reachable trade destinations */
    memset(bfsPrev, NONE, sizeof bfsPrev);
    bfsPrev[i] = i;
    bfsQ[qt++] = i;
    while (qh < qt) {
        cur = bfsQ[qh++];
        if (cur != i && trade_stop(&g_un[cur], p)) n++;
        for (k = 0; k < MAX_ADJ; k++) {
            uint8_t nb = adj[cur][k];
            if (nb == NONE || bfsPrev[nb] != NONE) continue;
            bfsPrev[nb] = cur;
            bfsQ[qt++] = nb;
        }
    }
    if (!n) return;
    pick = (uint8_t)rnd_n(n);
    for (k = 1; k < qt; k++) {
        cur = bfsQ[k];
        if (!trade_stop(&g_un[cur], p)) continue;
        if (!pick--) break;
    }
    hop = next_hop(i, cur);
    if (hop == NONE) return;
    t = new_unit(U_TRAIN, p, f->pos);
    if (!t) return;
    t->src = f->pos;
    t->tgt = g_un[hop].pos;
    t->dst = g_un[cur].pos;
    t->aux = (uint8_t)line_len(t->src, t->tgt);
    t->timer = 2;
    f->timer = 10;
}

static uint32_t train_gold(uint8_t owner, uint8_t stationOwner, uint16_t stops)
{
    int32_t g;
    if (stationOwner == owner)
        g = 10000;
    else if (ALLIED(owner, stationOwner))
        g = 35000;
    else
        g = 25000;
    if (stops > 9) g -= (int32_t)(stops - 9) * 5000;
    if (g < 5000) g = 5000;
    return gold_mul((uint32_t)g);
}

void train_tick(unit_t *u)
{
    int16_t k;
    unit_t *st;
    uint8_t hop;
    if (--u->timer) return;
    u->timer = 2;
    if (u->timer2 < u->aux) {
        u->timer2++;
        u->pos = line_point(u->src, u->tgt, u->timer2, u->aux);
        if (u->timer2 < u->aux) return;
    }
    /* arrived at the next station */
    k = unit_at(u->tgt);
    if (k < 0 || !is_station(&g_un[k])) {
        del_unit(u);
        return;
    }
    st = &g_un[k];
    if (trade_stop(st, u->owner)) {
        uint32_t g = train_gold(u->owner, st->owner, u->dist);
        g_pl[u->owner].gold += g;
        if (st->owner != u->owner) g_pl[st->owner].gold += g;
        u->dist++;
    }
    if (u->tgt == u->dst) {
        del_unit(u);
        return;
    }
    {
        int16_t d = unit_at(u->dst);
        if (d < 0 || !is_station(&g_un[d])) {
            del_unit(u);
            return;
        }
        hop = next_hop((uint8_t)k, (uint8_t)d);
    }
    if (hop == NONE) {
        del_unit(u);
        return;
    }
    u->src = u->tgt;
    u->tgt = g_un[hop].pos;
    u->timer2 = 0;
    u->aux = (uint8_t)line_len(u->src, u->tgt);
}
