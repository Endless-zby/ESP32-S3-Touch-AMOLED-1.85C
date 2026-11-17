#pragma once
#include <Arduino.h>

// The "小安" assistant: wake word -> cloud voice session -> tools -> sleep.
//
//   SLEEP  : screen off, WakeNet listening (Wi-Fi + SIP stay up)
//   AWAKE  : screen on, voice session with Qwen (continuous conversation)
//   CALL   : handed over to the SIP phone; resumes AWAKE when the call ends
//
// Callbacks let the sketch keep ownership of volume / brightness / dialing.
struct AssistantHooks {
  void (*setVolume)(int level) = nullptr;        // 0..100
  int  (*getVolume)() = nullptr;
  void (*setBrightness)(int level) = nullptr;    // 20..100
  int  (*getBrightness)() = nullptr;
  bool (*dialFrontDesk)() = nullptr;             // returns false when the phone cannot dial now
  bool (*phoneBusy)() = nullptr;                 // a SIP call is in progress
};

enum AssistantState { AS_SLEEP = 0, AS_WAKING, AS_SESSION, AS_FAILED, AS_CALL };

void assistant_begin(const AssistantHooks &hooks);
void assistant_loop();
void assistant_on_wake_word();                 // from WakeNet
void assistant_on_user_wake();                 // touch / BOOT while asleep (no chime)
void assistant_on_call_started();              // SIP call (in or out) took the audio
void assistant_on_call_ended();
void assistant_sleep_now();                    // force sleep (settings, etc.)
void assistant_touch_activity();               // user touched the screen: postpone the screen-off
AssistantState assistant_state();
