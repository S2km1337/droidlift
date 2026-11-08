#include "audio.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A sample counts as sound above about -66 dBFS (16-bit: |s| > 16). */
#define SILENCE_FLOAT (16.0f / 32768.0f)
#define SILENCE_S16 16

static SDL_AudioStream *stream;
static int frame_bytes, sample_rate;
/* Virtual clock: bytes pushed, and when the clock started. */
static uint64_t clock_pushed, clock_start_ns;
static int started;

static uint64_t n_samples, n_nonsilent, n_underruns;
static SDL_Mutex *stats_lock;

static void count(uint64_t samples, uint64_t nonsilent) {
  SDL_LockMutex(stats_lock);
  n_samples += samples;
  n_nonsilent += nonsilent;
  SDL_UnlockMutex(stats_lock);
}

/* The device's final mix, in float. */
static void SDLCALL postmix(void *ud, const SDL_AudioSpec *spec, float *buf, int len) {
  (void)ud;
  (void)spec;
  int n = len / (int)sizeof(float);
  uint64_t loud = 0;
  for (int i = 0; i < n; i++)
    if (buf[i] > SILENCE_FLOAT || buf[i] < -SILENCE_FLOAT) loud++;
  count((uint64_t)n, loud);
}

int audio_open(int channels, int rate) {
  if (channels < 1 || channels > 2 || rate < 8000 || rate > 48000) {
    fprintf(stderr, "[audio] unsupported format: %d ch, %d Hz\n", channels, rate);
    return -1;
  }
  if (!stats_lock) stats_lock = SDL_CreateMutex();
  if (stream) SDL_DestroyAudioStream(stream);
  stream = NULL;
  frame_bytes = 2 * channels;
  sample_rate = rate;
  clock_pushed = 0;
  started = 0;
  const char *mode = getenv("DL_AUDIO");
  if (mode && !strcmp(mode, "off")) {
    fprintf(stderr, "[audio] DL_AUDIO=off: virtual clock, %d ch, %d Hz\n", channels, rate);
    return 0;
  }
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    fprintf(stderr, "[audio] no audio (%s): virtual clock\n", SDL_GetError());
    return 0;
  }
  SDL_AudioSpec spec = {SDL_AUDIO_S16LE, channels, rate};
  stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
  if (!stream) {
    fprintf(stderr, "[audio] no playback device (%s): virtual clock\n", SDL_GetError());
    return 0;
  }
  SDL_SetAudioPostmixCallback(SDL_GetAudioStreamDevice(stream), postmix, NULL);
  SDL_ResumeAudioStreamDevice(stream);
  fprintf(stderr, "[audio] SDL %s output, %d ch, %d Hz\n", SDL_GetCurrentAudioDriver(), channels,
          rate);
  return 0;
}

uint32_t audio_queued(void) {
  if (stream) {
    int q = SDL_GetAudioStreamQueued(stream);
    return q > 0 ? (uint32_t)q : 0;
  }
  if (!started) return 0;
  uint64_t played = (SDL_GetTicksNS() - clock_start_ns) * (uint64_t)sample_rate / 1000000000u *
                    (uint64_t)frame_bytes;
  return played >= clock_pushed ? 0 : (uint32_t)(clock_pushed - played);
}

void audio_put(const void *pcm, uint32_t bytes) {
  if (!stats_lock) return;
  if (started && audio_queued() == 0) {
    SDL_LockMutex(stats_lock);
    n_underruns++;
    SDL_UnlockMutex(stats_lock);
  }
  if (stream) {
    SDL_PutAudioStreamData(stream, pcm, (int)bytes);
  } else {
    /* Restart the clock after an underrun, as a device would play on. */
    if (!started || audio_queued() == 0) {
      clock_start_ns = SDL_GetTicksNS();
      clock_pushed = 0;
    }
    clock_pushed += bytes;
    const int16_t *s = pcm;
    uint64_t loud = 0;
    for (uint32_t i = 0; i < bytes / 2; i++)
      if (s[i] > SILENCE_S16 || s[i] < -SILENCE_S16) loud++;
    count(bytes / 2, loud);
  }
  started = 1;
}

void audio_get_stats(audio_stats *out) {
  memset(out, 0, sizeof *out);
  if (!stats_lock) return;
  SDL_LockMutex(stats_lock);
  out->samples = n_samples;
  out->nonsilent = n_nonsilent;
  out->underruns = n_underruns;
  SDL_UnlockMutex(stats_lock);
  out->device = stream != NULL;
}
