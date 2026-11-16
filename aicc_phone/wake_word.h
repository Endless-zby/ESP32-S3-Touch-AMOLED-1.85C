#pragma once
#include <Arduino.h>

// Local wake word + command word recognition (ESP-SR, all on device).
//
// Two-stage, the way Espressif intends it:
//   1. WakeNet9 "你好小安" (wn9_nihaoxiaoan_tts2) runs continuously while idle.
//   2. After a wake, the Chinese MultiNet7 command recogniser takes over the
//      microphone for the command window; it is idle (loaded, not running) the
//      rest of the time, so command phrases can never fire without a wake.
//
// The model pack (tools/srmodels/srmodels_cn.bin, built by tools/pack_models.sh)
// lives in the "model" flash partition. Command ids come from its
// fst/commands_cn.txt, one line per phrase; keep this list and that file in sync.
#define WAKE_CMD_VOL_UP       2   // sheng yin da dian      声音大点
#define WAKE_CMD_VOL_DOWN     3   // sheng yin xiao dian    声音小点
#define WAKE_CMD_VOL_UP2      4   // da dian sheng          大点声
#define WAKE_CMD_VOL_DOWN2    5   // xiao dian sheng        小点声
#define WAKE_CMD_CALL_FRONT   6   // gei qian tai da dian hua     给前台打电话
#define WAKE_CMD_CALL_FRONT2  7   // da dian hua gei qian tai     打电话给前台
#define WAKE_CMD_CALL_FRONT3  8   // hu jiao qian tai             呼叫前台
#define WAKE_CMD_BRI_UP       9   // ping mu liang yi dian        屏幕亮一点
#define WAKE_CMD_BRI_DOWN    10   // ping mu an yi dian           屏幕暗一点
#define WAKE_CMD_BRI_UP2     11   // liang yi dian                亮一点
#define WAKE_CMD_BRI_DOWN2   12   // an yi dian                   暗一点

// Pseudo command id reported by consume() when the wake word itself is heard.
#define WAKE_CMD_WAKE        1

namespace WakeWord {

enum Stage { STAGE_OFF = 0, STAGE_WAKE, STAGE_COMMAND };

struct Stats {
  bool ready = false;        // both models created
  bool listening = true;     // feed() is currently accepted
  Stage stage = STAGE_OFF;
  uint32_t chunks = 0;       // audio chunks processed
  uint32_t wakes = 0;        // wake word detections
  uint32_t commands = 0;     // accepted command detections
  uint32_t wnAvgUs = 0;      // WakeNet detect() average (since last stats())
  uint32_t wnMaxUs = 0;
  uint32_t mnAvgUs = 0;      // MultiNet detect() average (since last stats())
  uint32_t mnMaxUs = 0;
  char wakeModel[32] = {};   // e.g. "wn9_nihaoxiaoan_tts2"
  char wakeWord[32] = {};    // e.g. "nihaoxiaoan"
  char cmdModel[32] = {};    // e.g. "mn7_cn"
  char error[64] = {};       // last failure reason
};

bool begin();                            // load the model pack, create WakeNet + MultiNet
void setListening(bool on);              // pause everything while a call owns the microphone
bool listening();
// Switch between the two stages. The application calls startCommandWindow()
// when it accepts a wake (after the chime) and endCommandWindow() when the
// window closes or a command was executed and no follow-up is expected.
void startCommandWindow();
void endCommandWindow();
Stage stage();
// Feed 16 kHz mono PCM from the audio task. No-op until begin() succeeds.
void feed(const int16_t *pcm, int samples);
// Latest detection (WAKE_CMD_WAKE or a command id), read-and-cleared by the main loop.
bool consume(int &commandId, float &prob, char *phrase, size_t cap);
// Wake sensitivity: 0 = model default (90% mode), 1 = relaxed (-0.05), 2 = strict (95% mode).
void setSensitivity(uint8_t level);
Stats stats();

}  // namespace WakeWord
