/* fixed-point helpers: log2 / exp2 / pow, random numbers, distances
 * Written for the eZ80: no long 32-bit shift loops, 24-bit math where it fits. */
#include "sim.h"
#include "fx_tab.h"

/* Bob Jenkins' small fast 8-bit generator: only byte operations */
static uint8_t ra = 0xF1, rb = 0x37, rc = 0x91, rd = 0x5C;

static uint8_t rnd8(void)
{
    uint8_t e = (uint8_t)(ra - (uint8_t)((rb << 1) | (rb >> 7)));
    ra = (uint8_t)(rb ^ (uint8_t)((rc << 4) | (rc >> 4)));
    rb = (uint8_t)(rc + rd);
    rc = (uint8_t)(rd + e);
    rd = (uint8_t)(e + ra);
    return rd;
}

void rnd_seed(uint32_t s)
{
    uint8_t i;
    ra = 0xF1;
    rb = (uint8_t)s;
    rc = (uint8_t)(s >> 8);
    rd = (uint8_t)(s >> 16) ^ (uint8_t)(s >> 24);
    for (i = 0; i < 32; i++) rnd8();
}

uint16_t rnd(void) { return (uint16_t)(rnd8() | ((uint16_t)rnd8() << 8)); }

uint16_t rnd_n(uint16_t n)
{
    if (n < 2) return 0;
    if (n <= 256) return (uint16_t)(((uint24_t)rnd() * n) >> 16); /* fits in 24 bits */
    return (uint16_t)(((uint32_t)rnd() * n) >> 16);
}

typedef union {
    uint32_t v;
    uint8_t b[4];
} u32b_t;

/* log2(x) in Q8; log2(0) = log2(1) = 0 */
uint16_t fx_log2(uint32_t x)
{
    u32b_t u;
    u16b_t w, r;
    uint8_t k = 3, msb;
    if (x < 2) return 0;
    u.v = x;
    while (!u.b[k]) k--;
    w.b[1] = u.b[k];
    w.b[0] = k ? u.b[k - 1] : 0;
    msb = (uint8_t)(k * 8 + 7);
    while (!(w.b[1] & 0x80)) {
        w.v += w.v;
        msb--;
    }
    /* the 8 bits after the leading one: bits 14..7 */
    r.b[1] = msb;
    r.b[0] = fx_logtab[(uint8_t)((uint8_t)(w.b[1] + w.b[1]) | (w.b[0] >> 7))];
    return r.v;
}

/* 2^(l/256), l may be negative (result rounds to 0) */
uint32_t fx_exp2(int16_t l)
{
    u16b_t u;
    u32b_t m;
    uint8_t ip;
    if (l < 0) return 0;
    u.v = (uint16_t)l;
    ip = u.b[1];
    m.v = fx_exptab[u.b[0]]; /* Q15 mantissa */
    if (ip >= 15) {
        if (ip > 30) return 0xFFFFFFFFu;
        for (ip -= 15; ip; ip--) m.v += m.v;
        return m.v;
    }
    ip = (uint8_t)(15 - ip);
    while (ip >= 8) {
        m.b[0] = m.b[1];
        m.b[1] = m.b[2];
        m.b[2] = m.b[3];
        m.b[3] = 0;
        ip -= 8;
    }
    for (; ip; ip--) m.v >>= 1;
    return m.v;
}

uint32_t fx_pow(uint32_t x, uint8_t p8)
{
    if (x == 0) return 0;
    return fx_exp2((int16_t)(((uint24_t)fx_log2(x) * p8) >> 8));
}

uint16_t isqrt32(uint32_t v)
{
    uint32_t r = 0, b = 1UL << 30;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else
            r >>= 1;
        b >>= 2;
    }
    return (uint16_t)r;
}

uint16_t tdist(tile_t a, tile_t b)
{
    int16_t dx = (int16_t)tx(a) - (int16_t)tx(b);
    int16_t dy = (int16_t)ty(a) - (int16_t)ty(b);
    return isqrt32((uint32_t)((int32_t)dx * dx + (int32_t)dy * dy));
}

uint16_t mdist(tile_t a, tile_t b)
{
    int16_t dx = (int16_t)tx(a) - (int16_t)tx(b);
    int16_t dy = (int16_t)ty(a) - (int16_t)ty(b);
    return (uint16_t)((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy));
}

bool near(tile_t a, tile_t b, uint8_t r)
{
    int16_t dx = (int16_t)tx(a) - (int16_t)tx(b);
    int16_t dy = (int16_t)ty(a) - (int16_t)ty(b);
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dx > r || dy > r) return false;
    return (uint16_t)(dx * dx + dy * dy) <= (uint16_t)r * r;
}
