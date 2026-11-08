/*
 * Software PVRTC1 4bpp decoder.
 *
 * The game ships its textures as PVRTC (the iOS1536_l/iOS768_l/iOS320_l
 * folders) and uploads them with glCompressedTexImage2D. Apple GPUs decode
 * PVRTC natively, but a host GL without GL_IMG_texture_compression_pvrtc
 * (the iOS Simulator's GPU) can't, so reimpl/gles.c decodes them to RGBA8
 * there.
 */
#ifndef PVRTC_H
#define PVRTC_H

#include <stddef.h>
#include <stdint.h>

#define GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG 0x8C00
#define GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG 0x8C01
#define GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG 0x8C02
#define GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG 0x8C03

/* Bytes of a 4bpp image of width x height pixels: at least 2x2 blocks. */
size_t pvrtc4_size(int width, int height);

/* Decodes a 4bpp image (power-of-two sides) into width*height RGBA8 pixels,
 * top row first as GL uploads it. Returns 0, or -1 if the sides aren't
 * powers of two or data is shorter than pvrtc4_size. */
int pvrtc4_decode(const uint8_t *data, size_t size, int width, int height, uint8_t *rgba);

#endif
