/*
 * Host audio output: one SDL3 playback stream for the game's PCM.
 *
 * The OpenSL ES shim (reimpl/opensles.c) pushes FMOD's buffers here and asks
 * how much is still queued, to pace FMOD's buffer callback. If no audio
 * device opens (or DL_AUDIO=off), a virtual clock drains the queue at the
 * stream's real rate instead, so FMOD runs the same with no sound.
 *
 * Stats count what the device's postmix callback receives (with a device),
 * or what is pushed (virtual clock), so a test can tell silence from sound.
 */
#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

/* Opens the output for signed 16-bit little-endian PCM. Returns 0 (with a
 * device or the virtual clock), or -1 for an unsupported format. Only one
 * output is open at a time; opening again replaces it. */
int audio_open(int channels, int rate);

/* Queues PCM bytes for playback. */
void audio_put(const void *pcm, uint32_t bytes);

/* Bytes queued and not yet played. */
uint32_t audio_queued(void);

typedef struct audio_stats {
  uint64_t samples;   /* received by the device (or pushed), all channels */
  uint64_t nonsilent; /* of those, above the silence threshold */
  uint64_t underruns; /* pushes that found the queue empty after the first */
  int device;         /* 1: a real SDL device, 0: the virtual clock */
} audio_stats;

void audio_get_stats(audio_stats *s);

#endif
