#pragma once
#include <Arduino.h>

// Local wake word only: ESP-SR WakeNet9 "你好小安" (wn9_nihaoxiaoan_tts2) runs
// continuously from the model pack in the "model" flash partition
// (tools/srmodels/srmodels_cn.bin, built by tools/pack_models.sh).
// Everything after the wake word is handled by the cloud voice session; the
// MultiNet command recogniser is no longer loaded.
namespace WakeWord {

struct Stats {
  bool ready = false;
  bool listening = true;
  uint32_t chunks = 0;
  uint32_t wakes = 0;
  uint32_t avgUs = 0;        // detect() average since last stats()
  uint32_t maxUs = 0;
  char model[32] = {};
  char word[32] = {};
  char error[64] = {};
};

bool begin();
void setListening(bool on);              // pause while a call / voice session owns the microphone
bool listening();
void feed(const int16_t *pcm, int samples);   // 16 kHz mono, from the audio task
bool consume();                          // true once per detection (read-and-clear)
void setSensitivity(uint8_t level);      // 0 default, 1 relaxed (-0.05), 2 strict (+0.01)
Stats stats();

}  // namespace WakeWord
