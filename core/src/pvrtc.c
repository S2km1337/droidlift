/*
 * PVRTC1 4bpp, decoded the way the format is specified: every 4x4 block
 * carries two low-resolution colours (A and B) and a 2-bit modulation value
 * per pixel. A pixel's A and B are the bilinear blend of the four blocks
 * whose centres surround it (wrapping at the edges), and its colour is A and
 * B mixed by its modulation weight out of 8. Blocks are stored in Morton
 * order with y in the low bit.
 */
#include "pvrtc.h"

#include <string.h>

typedef struct { int r, g, b, a; } rgba5; /* 5-bit colour, 4-bit alpha */

size_t pvrtc4_size(int width, int height) {
  int bw = width / 4 < 2 ? 2 : width / 4, bh = height / 4 < 2 ? 2 : height / 4;
  return (size_t)bw * bh * 8;
}

static int is_pow2(int v) { return v > 0 && !(v & (v - 1)); }

/* Morton index of block (x, y) in a w x h block grid; the longer side's
 * extra high bits follow the interleaved ones. */
static uint32_t twiddle(int w, int h, uint32_t x, uint32_t y) {
  uint32_t min = (uint32_t)(w < h ? w : h), rest = w < h ? y : x, out = 0;
  int shift = 0;
  for (uint32_t bit = 1; bit < min; bit <<= 1, shift++) {
    if (y & bit) out |= 1u << (2 * shift);
    if (x & bit) out |= 2u << (2 * shift);
  }
  return out | (rest >> shift) << (2 * shift);
}

/* Colour A (bits 1-15 of the colour word) or B (bits 16-31). Bit 15/31 set
 * means opaque RGB 555 (A's blue has 4 bits); clear means ARGB 3444 (A's
 * blue has 3 bits). Short fields repeat their top bits. */
static rgba5 colour(uint32_t word, int which_b) {
  uint32_t c = which_b ? word >> 16 : word & 0xffff;
  rgba5 o;
  if (c & 0x8000) {
    o.r = (c >> 10) & 0x1f;
    o.g = (c >> 5) & 0x1f;
    o.b = which_b ? c & 0x1f : (c & 0x1e) | ((c >> 4) & 1);
    o.a = 0xf;
  } else {
    o.a = (c >> 11) & 0xe;
    o.r = ((c >> 7) & 0x1e) | ((c >> 11) & 1);
    o.g = ((c >> 3) & 0x1e) | ((c >> 7) & 1);
    o.b = which_b ? ((c << 1) & 0x1e) | ((c >> 3) & 1) : ((c << 1) & 0x1c) | ((c >> 2) & 3);
  }
  return o;
}

int pvrtc4_decode(const uint8_t *data, size_t size, int width, int height, uint8_t *rgba) {
  if (!is_pow2(width) || !is_pow2(height) || size < pvrtc4_size(width, height)) return -1;
  int bw = width / 4 < 2 ? 2 : width / 4, bh = height / 4 < 2 ? 2 : height / 4;
#define BLOCK(bx, by) (data + 8 * (size_t)twiddle(bw, bh, (uint32_t)(bx), (uint32_t)(by)))
#define WORD(p, i) ((uint32_t)(p)[4 * (i)] | (uint32_t)(p)[4 * (i) + 1] << 8 | \
                    (uint32_t)(p)[4 * (i) + 2] << 16 | (uint32_t)(p)[4 * (i) + 3] << 24)
  for (int py = 0; py < height; py++) {
    /* The blocks whose centres (offset 2) bracket this row, and the weight. */
    int y0 = (py - 2) >> 2, fy = (py - 2) & 3;
    int by0 = (y0 + bh) % bh, by1 = (y0 + 1 + bh) % bh;
    for (int px = 0; px < width; px++) {
      int x0 = (px - 2) >> 2, fx = (px - 2) & 3;
      int bx0 = (x0 + bw) % bw, bx1 = (x0 + 1 + bw) % bw;
      const uint8_t *q[4] = {BLOCK(bx0, by0), BLOCK(bx1, by0), BLOCK(bx0, by1), BLOCK(bx1, by1)};
      int wt[4] = {(4 - fx) * (4 - fy), fx * (4 - fy), (4 - fx) * fy, fx * fy};
      int ch[2][4] = {{0}};
      for (int k = 0; k < 4; k++)
        for (int ab = 0; ab < 2; ab++) {
          rgba5 c = colour(WORD(q[k], 1), ab);
          ch[ab][0] += c.r * wt[k];
          ch[ab][1] += c.g * wt[k];
          ch[ab][2] += c.b * wt[k];
          ch[ab][3] += c.a * wt[k];
        }
      /* Weights sum to 16: 5-bit colour -> 8 bits, 4-bit alpha -> 8 bits. */
      int a8[4], b8[4];
      for (int i = 0; i < 3; i++) {
        a8[i] = (ch[0][i] >> 1) + (ch[0][i] >> 6);
        b8[i] = (ch[1][i] >> 1) + (ch[1][i] >> 6);
      }
      a8[3] = ch[0][3] + (ch[0][3] >> 4);
      b8[3] = ch[1][3] + (ch[1][3] >> 4);

      const uint8_t *own = BLOCK(px >> 2, py >> 2);
      int code = (WORD(own, 0) >> (2 * ((py & 3) * 4 + (px & 3)))) & 3;
      int punch = WORD(own, 1) & 1;
      static const int standard[4] = {0, 3, 5, 8}, punchthrough[4] = {0, 4, 4, 8};
      int m = punch ? punchthrough[code] : standard[code];
      uint8_t *o = rgba + 4 * ((size_t)py * width + px);
      for (int i = 0; i < 4; i++) o[i] = (uint8_t)((a8[i] * (8 - m) + b8[i] * m) / 8);
      if (punch && code == 2) o[3] = 0;
    }
  }
#undef BLOCK
#undef WORD
  return 0;
}
