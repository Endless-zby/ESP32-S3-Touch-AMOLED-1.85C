#pragma once
#include <Arduino.h>
#include "rtp.h"

enum AudioMode {
  AUDIO_IDLE = 0,
  AUDIO_RINGBACK,   // caller hears ringback while callee rings
  AUDIO_RINGING,    // local ring tone for an incoming call
  AUDIO_BUSY,       // busy / error tone
  AUDIO_CALL,       // full duplex RTP
};

bool audio_init();
void audio_start_task(RtpSession *rtp);
void audio_set_mode(AudioMode m);
AudioMode audio_mode();
void audio_set_volume(uint8_t percent);   // 0..100 (speaker)
void audio_set_mic_gain(uint8_t percent); // 0..200
void audio_set_aec_mode(uint8_t mode);    // 0 off, 1 low cost, 2 high performance (V2 only)
uint8_t audio_aec_mode();
void audio_set_slots(uint8_t micSlot, uint8_t refSlot); // V2: mic TDM slot 0..3; ref 0..3 = TDM slot, 4 = software reference
void audio_set_ref_delay(uint16_t ms);     // software reference delay
void audio_set_aec_filter(uint8_t n);      // esp-sr filter_length (echo tail = n x 16 ms)
void audio_set_mute(bool mute);
bool audio_is_muted();
void audio_click();                       // short key click
// 0..100 recent mic / speaker levels, for UI meters
uint8_t audio_mic_level();
uint8_t audio_spk_level();
