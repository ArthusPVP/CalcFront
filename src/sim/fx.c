/* fixed-point helpers: log2 / exp2 / pow, random numbers, distances
 * Written for the eZ80: no long 32-bit shift loops, 24-bit math where it fits. */
#include "sim.h"
#include "fx_tab.h"

/* Bob Jenkins' small fast 8-bit generator: only byte operations */
static uint8_t ra = 0xF1, rb = 0x37, rc = 0x91, rd = 0x5C;

FORCE_INLINE uint8_t rnd8(void)
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

void rnd_get(uint8_t *s)
{
    s[0] = ra;
    s[1] = rb;
    s[2] = rc;
    s[3] = rd;
}

void rnd_set(const uint8_t *s)
{
    ra = s[0];
    rb = s[1];
    rc = s[2];
    rd = s[3];
}

/* two calls, in a fixed order: inside one expression the compiler may make
 * them in either order, and the PC and the calculator would then differ */
uint16_t rnd(void)
{
    u16b_t r;
    r.b[0] = rnd8();
    r.b[1] = rnd8();
    return r.v;
}

/* a random tile in the box x0..x0+w-1, y0..y0+h-1 (x drawn first) */
tile_t rnd_tile(uint16_t x0, uint16_t w, uint16_t y0, uint16_t h)
{
    uint16_t x = (uint16_t)(x0 + rnd_n(w));
    return TILE(x, y0 + rnd_n(h));
}

uint16_t rnd_n(uint16_t n)
{
    u16b_t r;
    if (n < 2) return 0;
    r.v = rnd();
    if (n <= 256) return (uint16_t)(((uint24_t)r.v * n) >> 16); /* fits in 24 bits */
    /* (r * n) >> 16 with r = hi * 256 + lo, in 24-bit steps: the dropped low
     * byte of lo * n can never carry into the result */
    return (uint16_t)(((uint24_t)r.b[1] * n + (((uint24_t)r.b[0] * n) >> 8)) >> 8);
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
    uint24_t m;
    uint8_t ip;
    if (l < 0) return 0;
    u.v = (uint16_t)l;
    ip = u.b[1];
    m = fx_exptab[u.b[0]]; /* Q15 mantissa, 16 bits */
    if (ip >= 15) {
        if (ip > 30) return 0xFFFFFFFFu;
        return (uint32_t)m << (ip - 15);
    }
    return m >> (15 - ip); /* one 24-bit shift instead of a 32-bit loop */
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

/* dx, dy between two tiles: one row lookup per tile (tx() alone does one) */
FORCE_INLINE void delta(tile_t a, tile_t b, int *dx, int *dy)
{
    int ya = ty(a), yb = ty(b);
    *dx = (int)(a - g_rowStart[ya]) - (int)(b - g_rowStart[yb]);
    *dy = ya - yb;
}

uint16_t tdist(tile_t a, tile_t b)
{
    int dx, dy;
    delta(a, b, &dx, &dy);
    return isqrt32((uint32_t)((int32_t)dx * dx + (int32_t)dy * dy));
}

uint16_t mdist(tile_t a, tile_t b)
{
    int dx, dy;
    delta(a, b, &dx, &dy);
    return (uint16_t)((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy));
}

bool near(tile_t a, tile_t b, uint8_t r)
{
    int dx, dy;
    delta(a, b, &dx, &dy);
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dx > r || dy > r) return false;
    return (uint16_t)(dx * dx + dy * dy) <= (uint16_t)r * r;
}
