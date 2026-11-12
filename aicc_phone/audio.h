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
void audio_set_mute(bool mute);
bool audio_is_muted();
void audio_click();                       // short key click
// 0..100 recent mic / speaker levels, for UI meters
uint8_t audio_mic_level();
uint8_t audio_spk_level();
