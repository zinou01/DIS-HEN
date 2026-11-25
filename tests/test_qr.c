/* test_qr.c - structural checks for the QR encoder (installer/qr.c).
 *
 * Full correctness (that a phone can actually read it) was verified by
 * decoding 127 generated symbols with an independent reader (OpenCV); this
 * test pins the properties that must never regress and needs no extra
 * dependencies, so CI can run it. */
#include "qr.h"
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c, name) do { if(!(c)){ printf("FAIL %s\n", name); g_fail++; } } while(0)

static uint8_t m[QR_MAX_MODULES];

static int at(int n, int x, int y){ return m[y * n + x]; }

static void check_finder(int n, int ox, int oy, const char *name){
    int ok = 1;
    for(int y = 0; y < 7; y++)
        for(int x = 0; x < 7; x++){
            int edge = (x == 0 || x == 6 || y == 0 || y == 6);
            int core = (x >= 2 && x <= 4 && y >= 2 && y <= 4);
            if(at(n, ox + x, oy + y) != (edge || core)) ok = 0;
        }
    CHECK(ok, name);
}

/* BCH(15,5) format word: valid iff the 15 bits XOR the mask have remainder 0
 * against the generator 0x537. */
static int format_ok(int n, int *mask_out){
    int bits = 0;
    for(int i = 0; i <= 5; i++) bits |= at(n, 8, i) << i;
    bits |= at(n, 8, 7) << 6;
    bits |= at(n, 8, 8) << 7;
    bits |= at(n, 7, 8) << 8;
    for(int i = 9; i < 15; i++) bits |= at(n, 14 - i, 8) << i;
    bits ^= 0x5412;
    int rem = bits;
    for(int i = 14; i >= 10; i--) if((rem >> i) & 1) rem ^= 0x537 << (i - 10);
    *mask_out = (bits >> 10) & 7;
    /* level M must read 00 in the top two data bits of the five */
    return rem == 0 && ((bits >> 13) & 3) == 0;
}

int main(void){
    /* the real use: a Discord login link */
    const char *url = "https://discord.com/ra/AbCdEfGhIjKlMnOpQrStUvWxYz0123456789_-abcdefgh";
    int n = qr_encode(url, m, sizeof m);
    CHECK(n == 17 + 4 * 5, "link fits version 5");  /* 69 bytes; v5 holds 84 at level M */
    check_finder(n, 0, 0, "finder top-left");
    check_finder(n, n - 7, 0, "finder top-right");
    check_finder(n, 0, n - 7, "finder bottom-left");
    int ok = 1;
    for(int i = 8; i < n - 8; i++)
        if(at(n, i, 6) != (i % 2 == 0) || at(n, 6, i) != (i % 2 == 0)) ok = 0;
    CHECK(ok, "timing patterns");
    CHECK(at(n, 8, n - 8) == 1, "dark module");
    int mask = -1;
    CHECK(format_ok(n, &mask), "format info valid (BCH, level M)");

    /* size grows with the payload, stays within versions 1..10 */
    CHECK(qr_encode("a", m, sizeof m) == 21, "1 byte -> version 1");
    CHECK(qr_encode("0123456789012345678901234", m, sizeof m) == 25, "25 bytes -> version 2 (holds 26)");
    CHECK(qr_encode("01234567890123", m, sizeof m) == 21, "14 bytes -> version 1 (holds 14)");
    CHECK(qr_encode("012345678901234", m, sizeof m) == 25, "15 bytes -> version 2");
    char big[300];
    memset(big, 'x', sizeof big); big[213] = 0;
    CHECK(qr_encode(big, m, sizeof m) == 57, "213 bytes -> version 10");
    big[214] = 'x'; big[214] = 0; big[213] = 'x';
    CHECK(qr_encode(big, m, sizeof m) == 0, "214 bytes does not fit");

    /* bad arguments are refused, never crash */
    CHECK(qr_encode("", m, sizeof m) == 0, "empty refused");
    CHECK(qr_encode(NULL, m, sizeof m) == 0, "NULL text refused");
    CHECK(qr_encode("a", NULL, sizeof m) == 0, "NULL out refused");
    CHECK(qr_encode("a", m, 10) == 0, "small buffer refused");

    /* deterministic */
    static uint8_t m2[QR_MAX_MODULES];
    qr_encode(url, m, sizeof m);
    qr_encode(url, m2, sizeof m2);
    CHECK(memcmp(m, m2, (size_t)n * n) == 0, "deterministic");

    if(g_fail){ printf("qr tests FAILED (%d)\n", g_fail); return 1; }
    printf("qr tests passed\n");
    return 0;
}
