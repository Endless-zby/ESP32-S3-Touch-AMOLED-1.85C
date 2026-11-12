#pragma once
#include <Arduino.h>

// Local wake word / command word recognition.
//
// Espressif does not ship a WakeNet model for a custom phrase, so the reference
// project (xiaoxi-claude/HotelPet) uses the Chinese MultiNet7 command recogniser
// instead: the model pack in the "model" flash partition carries a command table
// (fst/commands_cn.txt) whose entries are pinyin, currently
//   "xiao xi xiao xi"  ->  "小西小西"
// and this module feeds 16 kHz mono microphone samples straight into it.
//
// Phase 1 only reports detections over the serial console; the UI/command
// dispatch is layered on top later.
namespace WakeWord {

struct Stats {
  bool ready = false;        // model pack loaded and MultiNet created
  bool listening = true;     // feed() is currently accepted
  uint32_t chunks = 0;       // 16 kHz chunks sent to MultiNet
  uint32_t detections = 0;   // accepted detections (prob >= threshold)
  uint32_t maxDetectUs = 0;  // longest MultiNet detect() call
  char model[32] = {};       // e.g. "mn7_cn"
  char error[64] = {};       // last failure reason
};

bool begin();                            // load the model pack, create MultiNet
void setListening(bool on);              // pause while a call owns the CPU
bool listening();
// Feed 16 kHz mono PCM. Called from the audio task; no-op until begin() succeeds.
void feed(const int16_t *pcm, int samples);
// Latest detection, read-and-cleared by the main loop.
bool consume(int &commandId, float &prob, char *phrase, size_t cap);
Stats stats();

}  // namespace WakeWord
