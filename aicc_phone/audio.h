#pragma once
#include <Arduino.h>
#include "rtp.h"

enum AudioMode {
  AUDIO_IDLE = 0,
  AUDIO_RINGBACK,   // caller hears ringback while callee rings
  AUDIO_RINGING,    // local ring tone for an incoming call
  AUDIO_CALL,       // full duplex RTP
  AUDIO_VOICE,      // full duplex with the cloud voice session (VoiceSession rings)
};

bool audio_init();
void audio_start_task(RtpSession *rtp);
void audio_set_mode(AudioMode m);
AudioMode audio_mode();
void audio_set_volume(uint8_t percent);   // 0..100 (speaker)
void audio_set_mic_gain(uint8_t percent); // 0..200
void audio_set_aec_mode(uint8_t mode);    // 0 off, 1 FD low cost (default), 2 VoIP high perf, 3 FD high perf (V2 only)
void audio_set_slots(uint8_t micSlot, uint8_t refSlot); // V2: mic TDM slot 0..3; ref 0..3 = TDM slot, 4 = software reference
void audio_set_ref_delay(uint16_t ms);     // software reference delay
void audio_set_aec_filter(uint8_t n);      // esp-sr filter_length (echo tail = n x 16 ms)
void audio_set_ref_gain(uint8_t code);     // ES7210 reference-channel PGA gain code 0..14 (10 = 30 dB)
void audio_set_mic_pga(uint8_t code);      // ES7210 MIC1/MIC2 PGA gain code 0..14
void audio_set_ref_dig_gain(uint8_t x);    // software gain on the reference (x1..x8)
void audio_set_res_suppress(bool on);      // residual echo suppression after the AEC
void audio_set_mute(bool mute);
void audio_click();                       // short key click
// Wake acknowledgement chime, rendered by the audio task while idle so the PA
// is only switched on briefly.
enum AudioPrompt {
  AUDIO_PROMPT_NONE = 0,
  AUDIO_PROMPT_WAKE,     // rising two-note chime
};
void audio_prompt(uint8_t kind);
bool audio_prompt_busy();
// 0..100 recent mic / speaker levels, for UI meters
uint8_t audio_mic_level();
uint8_t audio_spk_level();
