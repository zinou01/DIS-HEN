/* qr.c - QR Code encoder. See qr.h.
 *
 * Structure follows the ISO/IEC 18004 procedure: build the data bit stream,
 * split into Reed-Solomon blocks and interleave, place function patterns, lay
 * out the codewords in the zigzag, then pick the best of the eight masks. */
#include "qr.h"

#include <string.h>

/* ---- tables (versions 1..10, error correction level M) ---- */

static const int ECC_PER_BLOCK_M[QR_MAX_VERSION + 1] =
    { -1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26 };
static const int NUM_BLOCKS_M[QR_MAX_VERSION + 1] =
    { -1,  1,  1,  1,  2,  2,  4,  4,  4,  5,  5 };

/* ---- GF(256) / Reed-Solomon ---- */

static uint8_t gf_mul(uint8_t x, uint8_t y){
    int z = 0;
    for(int i = 7; i >= 0; i--){
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return (uint8_t)z;
}

static void rs_divisor(int degree, uint8_t *result){
    memset(result, 0, (size_t)degree);
    result[degree - 1] = 1;
    uint8_t root = 1;
    for(int i = 0; i < degree; i++){
        for(int j = 0; j < degree; j++){
            result[j] = gf_mul(result[j], root);
            if(j + 1 < degree) result[j] ^= result[j + 1];
        }
        root = gf_mul(root, 0x02);
    }
}

static void rs_remainder(const uint8_t *data, int len, const uint8_t *divisor,
                         int degree, uint8_t *result){
    memset(result, 0, (size_t)degree);
    for(int i = 0; i < len; i++){
        uint8_t factor = data[i] ^ result[0];
        memmove(result, result + 1, (size_t)degree - 1);
        result[degree - 1] = 0;
        for(int j = 0; j < degree; j++)
            result[j] ^= gf_mul(divisor[j], factor);
    }
}

/* ---- geometry ---- */

static int raw_data_modules(int ver){
    int r = (16 * ver + 128) * ver + 64;
    if(ver >= 2){
        int na = ver / 7 + 2;
        r -= (25 * na - 10) * na - 55;
        if(ver >= 7) r -= 36;
    }
    return r;
}

static int data_codewords(int ver){
    return raw_data_modules(ver) / 8 - ECC_PER_BLOCK_M[ver] * NUM_BLOCKS_M[ver];
}

static int align_positions(int ver, int *out){
    if(ver == 1) return 0;
    int size = ver * 4 + 17;
    int na = ver / 7 + 2;
    int step = (ver * 4 + na * 2 + 1) / (na * 2 - 2) * 2;
    out[0] = 6;
    for(int i = na - 1, pos = size - 7; i >= 1; i--, pos -= step) out[i] = pos;
    return na;
}

/* ---- drawing state ---- */

typedef struct {
    int size;
    uint8_t mod[QR_MAX_MODULES];
    uint8_t fn[QR_MAX_MODULES];     /* 1 = function module, not data */
} qr_t;

static void set_fn(qr_t *q, int x, int y, int dark){
    q->mod[y * q->size + x] = (uint8_t)(dark != 0);
    q->fn[y * q->size + x] = 1;
}

static void draw_finder(qr_t *q, int cx, int cy){
    for(int dy = -4; dy <= 4; dy++)
        for(int dx = -4; dx <= 4; dx++){
            int dist = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy)
                       ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
            int x = cx + dx, y = cy + dy;
            if(x >= 0 && x < q->size && y >= 0 && y < q->size)
                set_fn(q, x, y, dist != 2 && dist != 4);
        }
}

static void draw_align(qr_t *q, int cx, int cy){
    for(int dy = -2; dy <= 2; dy++)
        for(int dx = -2; dx <= 2; dx++){
            int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            int m = ax > ay ? ax : ay;
            set_fn(q, cx + dx, cy + dy, m != 1);
        }
}

static void draw_format(qr_t *q, int mask){
    /* error correction level M has format bits 00 */
    int data = (0 << 3) | mask;
    int rem = data;
    for(int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    int bits = ((data << 10) | rem) ^ 0x5412;
    for(int i = 0; i <= 5; i++) set_fn(q, 8, i, (bits >> i) & 1);
    set_fn(q, 8, 7, (bits >> 6) & 1);
    set_fn(q, 8, 8, (bits >> 7) & 1);
    set_fn(q, 7, 8, (bits >> 8) & 1);
    for(int i = 9; i < 15; i++) set_fn(q, 14 - i, 8, (bits >> i) & 1);
    for(int i = 0; i < 8; i++) set_fn(q, q->size - 1 - i, 8, (bits >> i) & 1);
    for(int i = 8; i < 15; i++) set_fn(q, 8, q->size - 15 + i, (bits >> i) & 1);
    set_fn(q, 8, q->size - 8, 1);   /* the always-dark module */
}

static void draw_function_patterns(qr_t *q, int ver){
    int size = q->size;
    for(int i = 0; i < size; i++){
        set_fn(q, 6, i, i % 2 == 0);
        set_fn(q, i, 6, i % 2 == 0);
    }
    draw_finder(q, 3, 3);
    draw_finder(q, size - 4, 3);
    draw_finder(q, 3, size - 4);
    int pos[8];
    int na = align_positions(ver, pos);
    for(int i = 0; i < na; i++)
        for(int j = 0; j < na; j++){
            if((i == 0 && j == 0) || (i == 0 && j == na - 1) ||
               (i == na - 1 && j == 0)) continue;
            draw_align(q, pos[i], pos[j]);
        }
    draw_format(q, 0);              /* reserve; redrawn with the real mask */
    if(ver >= 7){
        int rem = ver;
        for(int i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
        int bits = (ver << 12) | rem;
        for(int i = 0; i < 18; i++){
            int bit = (bits >> i) & 1;
            int a = size - 11 + i % 3, b = i / 3;
            set_fn(q, a, b, bit);
            set_fn(q, b, a, bit);
        }
    }
}

static void draw_codewords(qr_t *q, const uint8_t *cw, int ncw){
    int i = 0;
    for(int right = q->size - 1; right >= 1; right -= 2){
        if(right == 6) right = 5;
        for(int vert = 0; vert < q->size; vert++)
            for(int j = 0; j < 2; j++){
                int x = right - j;
                int upward = ((right + 1) & 2) == 0;
                int y = upward ? q->size - 1 - vert : vert;
                if(!q->fn[y * q->size + x] && i < ncw * 8){
                    q->mod[y * q->size + x] = (uint8_t)((cw[i >> 3] >> (7 - (i & 7))) & 1);
                    i++;
                }
            }
    }
}

static void apply_mask(qr_t *q, int mask){
    for(int y = 0; y < q->size; y++)
        for(int x = 0; x < q->size; x++){
            int inv;
            switch(mask){
            case 0: inv = (x + y) % 2 == 0; break;
            case 1: inv = y % 2 == 0; break;
            case 2: inv = x % 3 == 0; break;
            case 3: inv = (x + y) % 3 == 0; break;
            case 4: inv = (x / 3 + y / 2) % 2 == 0; break;
            case 5: inv = x * y % 2 + x * y % 3 == 0; break;
            case 6: inv = (x * y % 2 + x * y % 3) % 2 == 0; break;
            default: inv = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
            }
            if(inv && !q->fn[y * q->size + x]) q->mod[y * q->size + x] ^= 1;
        }
}

/* Penalty score per the four ISO rules (lower is better). */
static int penalty(const qr_t *q){
    int n = q->size, score = 0, dark = 0;
#define M(x, y) q->mod[(y) * n + (x)]
    for(int pass = 0; pass < 2; pass++){
        for(int a = 0; a < n; a++){
            int run = 1;
            for(int b = 1; b < n; b++){
                int cur  = pass ? M(a, b)     : M(b, a);
                int prev = pass ? M(a, b - 1) : M(b - 1, a);
                if(cur == prev){
                    run++;
                    if(run == 5) score += 3;
                    else if(run > 5) score++;
                } else run = 1;
            }
            /* finder-like 1:1:3:1:1 with a light margin on either side */
            for(int b = 0; b + 11 <= n; b++){
                static const uint8_t p1[11] = {1,0,1,1,1,0,1,0,0,0,0};
                static const uint8_t p2[11] = {0,0,0,0,1,0,1,1,1,0,1};
                int m1 = 1, m2 = 1;
                for(int k = 0; k < 11; k++){
                    int v = pass ? M(a, b + k) : M(b + k, a);
                    if(v != p1[k]) m1 = 0;
                    if(v != p2[k]) m2 = 0;
                }
                score += 40 * (m1 + m2);
            }
        }
    }
    for(int y = 0; y < n; y++)
        for(int x = 0; x < n; x++){
            dark += M(x, y);
            if(x + 1 < n && y + 1 < n && M(x, y) == M(x + 1, y) &&
               M(x, y) == M(x, y + 1) && M(x, y) == M(x + 1, y + 1))
                score += 3;
        }
#undef M
    int total = n * n;
    int diff = dark * 20 - total * 10;
    if(diff < 0) diff = -diff;
    int k = (diff + total - 1) / total - 1;
    return score + (k > 0 ? k : 0) * 10;
}

/* ---- bit stream ---- */

typedef struct { uint8_t b[512]; int len; } bits_t;

static void put_bits(bits_t *s, unsigned val, int n){
    for(int i = n - 1; i >= 0; i--){
        if((s->len >> 3) < (int)sizeof s->b){
            if(((val >> i) & 1) != 0) s->b[s->len >> 3] |= (uint8_t)(0x80 >> (s->len & 7));
        }
        s->len++;
    }
}

int qr_encode(const char *text, uint8_t *modules, size_t cap){
    if(!text || !modules || cap < QR_MAX_MODULES) return 0;
    size_t n = strlen(text);
    if(n == 0) return 0;

    /* smallest version whose capacity holds mode(4) + count + bytes */
    int ver = 0;
    for(int v = 1; v <= QR_MAX_VERSION; v++){
        int cc = v < 10 ? 8 : 16;
        if(4 + cc + (int)n * 8 <= data_codewords(v) * 8){ ver = v; break; }
    }
    if(!ver) return 0;

    int ncw = data_codewords(ver);
    static bits_t bs;
    memset(&bs, 0, sizeof bs);
    put_bits(&bs, 0x4, 4);                              /* byte mode */
    put_bits(&bs, (unsigned)n, ver < 10 ? 8 : 16);
    for(size_t i = 0; i < n; i++) put_bits(&bs, (uint8_t)text[i], 8);
    int cap_bits = ncw * 8;
    int term = cap_bits - bs.len;
    put_bits(&bs, 0, term < 4 ? term : 4);
    while(bs.len % 8) put_bits(&bs, 0, 1);
    for(int pad = 0xEC; bs.len < cap_bits; pad ^= 0xEC ^ 0x11) put_bits(&bs, (unsigned)pad, 8);

    /* split into blocks, add Reed-Solomon, interleave */
    int nb = NUM_BLOCKS_M[ver], ecc = ECC_PER_BLOCK_M[ver];
    int raw = raw_data_modules(ver) / 8;
    int short_blocks = nb - raw % nb;
    int short_len = raw / nb;
    uint8_t divisor[32];
    rs_divisor(ecc, divisor);

    static uint8_t blocks[5][256];
    int dlen_of[5] = {0};
    int k = 0;
    for(int i = 0; i < nb; i++){
        int dlen = short_len - ecc + (i < short_blocks ? 0 : 1);
        memcpy(blocks[i], bs.b + k, (size_t)dlen);
        rs_remainder(bs.b + k, dlen, divisor, ecc, blocks[i] + dlen);
        dlen_of[i] = dlen;
        k += dlen;
    }
    /* Interleave: every block's data bytes first, then every block's
     * error-correction bytes (ISO 18004 section 7.6). */
    static uint8_t cw[600];
    int w = 0;
    for(int i = 0; i < short_len - ecc + 1; i++)
        for(int j = 0; j < nb; j++)
            if(i < dlen_of[j]) cw[w++] = blocks[j][i];
    for(int i = 0; i < ecc; i++)
        for(int j = 0; j < nb; j++)
            cw[w++] = blocks[j][dlen_of[j] + i];
    if(w != raw) return 0;                              /* internal consistency */

    /* build the symbol and choose the mask with the lowest penalty */
    static qr_t q;
    memset(&q, 0, sizeof q);
    q.size = ver * 4 + 17;
    draw_function_patterns(&q, ver);
    draw_codewords(&q, cw, raw);
    int best = 0, best_pen = 1 << 30;
    for(int m = 0; m < 8; m++){
        apply_mask(&q, m);
        draw_format(&q, m);
        int pen = penalty(&q);
        if(pen < best_pen){ best_pen = pen; best = m; }
        apply_mask(&q, m);                              /* XOR again = undo */
    }
    apply_mask(&q, best);
    draw_format(&q, best);

    memcpy(modules, q.mod, (size_t)q.size * (size_t)q.size);
    return q.size;
}
