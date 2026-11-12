#pragma once
#include <stdint.h>
#include <stddef.h>

void g711_init();
uint8_t linear_to_ulaw(int16_t s);
uint8_t linear_to_alaw(int16_t s);
int16_t ulaw_to_linear(uint8_t u);
int16_t alaw_to_linear(uint8_t a);

void g711_encode(const int16_t *pcm, uint8_t *out, size_t n, bool alaw);
void g711_decode(const uint8_t *in, int16_t *pcm, size_t n, bool alaw);
