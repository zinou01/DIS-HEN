/* qr.h - minimal QR Code encoder (byte mode, error correction M, versions 1-10).
 *
 * Pure C with no platform dependencies, so it is unit-tested on a PC and
 * decoded back with an independent reader. Enough for the Discord login link
 * (about 70 bytes -> version 5). Model 2 only; no Kanji/numeric/alnum modes. */
#ifndef DISHEN_QR_H
#define DISHEN_QR_H

#include <stddef.h>
#include <stdint.h>

#define QR_MAX_VERSION 10
#define QR_MAX_SIZE    (17 + 4 * QR_MAX_VERSION)      /* 57 */
#define QR_MAX_MODULES (QR_MAX_SIZE * QR_MAX_SIZE)

/* Encode `text` (any bytes, NUL-terminated) into `modules` (row-major,
 * 1 = dark, 0 = light). `cap` must be at least QR_MAX_MODULES.
 * Returns the side length in modules (21..57), or 0 when the text does not fit
 * or the arguments are unusable. The mask is chosen by penalty score. */
int qr_encode(const char *text, uint8_t *modules, size_t cap);

#endif
