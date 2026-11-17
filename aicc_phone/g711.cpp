// G.711 mu-law / A-law, derived from the public domain Sun Microsystems g711.c
#include "g711.h"

#define BIAS 0x84
#define CLIP 32635

static int16_t s_ulaw_tab[256];
static int16_t s_alaw_tab[256];
static bool s_init = false;

static int16_t ulaw2lin(uint8_t u) {
  u = ~u;
  int t = ((u & 0x0F) << 3) + BIAS;
  t <<= ((unsigned)u & 0x70) >> 4;
  return (u & 0x80) ? (BIAS - t) : (t - BIAS);
}

static int16_t alaw2lin(uint8_t a) {
  a ^= 0x55;
  int t = (a & 0x0F) << 4;
  int seg = ((unsigned)a & 0x70) >> 4;
  switch (seg) {
    case 0: t += 8; break;
    case 1: t += 0x108; break;
    default: t += 0x108; t <<= seg - 1;
  }
  return (a & 0x80) ? t : -t;
}

void g711_init() {
  if (s_init) return;
  for (int i = 0; i < 256; i++) {
    s_ulaw_tab[i] = ulaw2lin((uint8_t)i);
    s_alaw_tab[i] = alaw2lin((uint8_t)i);
  }
  s_init = true;
}

static const int16_t seg_uend[8] = {0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF};
static const int16_t seg_aend[8] = {0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF};

static inline int search(int val, const int16_t *table, int size) {
  for (int i = 0; i < size; i++) if (val <= table[i]) return i;
  return size;
}

uint8_t linear_to_ulaw(int16_t pcm) {
  int mask, seg;
  int v = pcm >> 2;
  if (v < 0) { v = -v; mask = 0x7F; } else { mask = 0xFF; }
  if (v > CLIP >> 2) v = CLIP >> 2;
  v += (BIAS >> 2);
  seg = search(v, seg_uend, 8);
  if (seg >= 8) return (uint8_t)(0x7F ^ mask);
  uint8_t u = (seg << 4) | ((v >> (seg + 1)) & 0x0F);
  return u ^ mask;
}

uint8_t linear_to_alaw(int16_t pcm) {
  int mask, seg;
  int v = pcm >> 3;
  if (v >= 0) mask = 0xD5; else { mask = 0x55; v = -v - 1; }
  seg = search(v, seg_aend, 8);
  if (seg >= 8) return (uint8_t)(0x7F ^ mask);
  uint8_t a = seg << 4;
  if (seg < 2) a |= (v >> 1) & 0x0F; else a |= (v >> seg) & 0x0F;
  return a ^ mask;
}

void g711_encode(const int16_t *pcm, uint8_t *out, size_t n, bool alaw) {
  if (alaw) for (size_t i = 0; i < n; i++) out[i] = linear_to_alaw(pcm[i]);
  else for (size_t i = 0; i < n; i++) out[i] = linear_to_ulaw(pcm[i]);
}

void g711_decode(const uint8_t *in, int16_t *pcm, size_t n, bool alaw) {
  const int16_t *t = alaw ? s_alaw_tab : s_ulaw_tab;
  for (size_t i = 0; i < n; i++) pcm[i] = t[in[i]];
}
