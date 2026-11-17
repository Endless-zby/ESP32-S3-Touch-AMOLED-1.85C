#include "assistant.h"
#include "config.h"
#include "settings.h"
#include "audio.h"
#include "ui.h"
#include "ui_strings.h"
#include "wake_word.h"
#include "voice_session.h"
#include "face_model.h"
#include <WiFi.h>

namespace {

AssistantHooks H;
AssistantState s_state = AS_SLEEP;
uint32_t s_stateSince = 0;
uint32_t s_sessionStartAt = 0;
uint32_t s_lastActivity = 0;       // last speech by either side
uint32_t s_failedAt = 0;
bool s_resumeAfterCall = false;    // a session was running when the call started
bool s_pendingDial = false;        // call_front_desk accepted: dial after the model finishes its sentence
uint32_t s_pendingDialAt = 0;
bool s_greetAfterCall = false;
uint32_t s_lastSpeakingUi = 0;
bool s_ringOn = false;

const char *kToolsJson =
  "["
   "{\"type\":\"function\",\"name\":\"set_volume\",\"description\":\"调节这台设备的扬声器音量。客人说大声点/小声点/静音/音量调到多少时调用。\","
     "\"parameters\":{\"type\":\"object\",\"properties\":{"
       "\"level\":{\"type\":\"integer\",\"description\":\"目标音量 0-100，与 delta 二选一\"},"
       "\"delta\":{\"type\":\"integer\",\"description\":\"相对调整量，正数调大负数调小，一般用 ±20\"}}}},"
   "{\"type\":\"function\",\"name\":\"set_brightness\",\"description\":\"调节这台设备屏幕亮度。客人说太亮/太暗/亮一点/暗一点时调用。\","
     "\"parameters\":{\"type\":\"object\",\"properties\":{"
       "\"level\":{\"type\":\"integer\",\"description\":\"目标亮度 20-100，与 delta 二选一\"},"
       "\"delta\":{\"type\":\"integer\",\"description\":\"相对调整量，正数调亮负数调暗，一般用 ±20\"}}}},"
   "{\"type\":\"function\",\"name\":\"call_front_desk\",\"description\":\"用这台话机拨打酒店前台。客人要找前台、找人工、要服务员、送东西、修东西、投诉、退房、叫车等需要真人处理的事情时调用。调用后告诉客人正在接通，然后停止说话。\","
     "\"parameters\":{\"type\":\"object\",\"properties\":{\"reason\":{\"type\":\"string\",\"description\":\"客人的诉求，一句话\"}}}},"
   "{\"type\":\"function\",\"name\":\"end_conversation\",\"description\":\"客人表示不需要帮助了、说再见/没事了/谢谢不用了时调用，设备随后进入休眠。先简短道别再调用。\","
     "\"parameters\":{\"type\":\"object\",\"properties\":{}}}"
  "]";

String buildInstructions() {
  String s;
  s.reserve(900 + g_settings.aiExtra.length());
  s += "你是酒店客房里的语音助手“小安”，通过房间里一台圆形小话机和客人对话。";
  s += "规则：1）回答简短、口语化，一般不超过两句话，不要列清单。";
  s += "2）客人要调音量、调亮度时直接调用对应工具，完成后用一句话确认。";
  s += "3）客人要找前台、人工、服务员，或有送物、维修、投诉、退房、叫车等需要人处理的事，调用 call_front_desk，告诉客人正在接通，然后不要再说话。";
  s += "4）客人说再见、没事了、谢谢不用了，先简短道别，再调用 end_conversation。";
  s += "5）不确定的信息不要编造，建议客人联系前台。";
  s += "6）用客人使用的语言回答，默认普通话。";
  if (g_settings.aiExtra.length()) { s += "酒店信息："; s += g_settings.aiExtra; }
  return s;
}

void setRing(bool on) {
  if (s_ringOn == on) return;
  s_ringOn = on;
  ui_face_listening(on);
}

void enterState(AssistantState st) {
  if (s_state == st) return;
  Serial.printf("[ASSIST] %d -> %d\n", (int)s_state, (int)st);
  s_state = st;
  s_stateSince = millis();
}

void goSleep() {
  VoiceSession::end();
  if (audio_mode() == AUDIO_VOICE) audio_set_mode(AUDIO_IDLE);
  setRing(false);
  ui_face_caption("");
  ui_face_set_expr(EX_SLEEPY);
  ui_screen_sleep(true);
  WakeWord::setListening(true);
  enterState(AS_SLEEP);
}

bool startSession(const char *greeting) {
  if (WiFi.status() != WL_CONNECTED) { ui_toast(STR_NO_NETWORK, 2500); return false; }
  if (g_settings.aiKey.length() == 0) { ui_toast(STR_AI_NO_KEY, 3000); return false; }
  VoiceSession::Config c;
  c.host = g_settings.aiHost;
  c.model = g_settings.aiModel;
  c.apiKey = g_settings.aiKey;
  c.voice = g_settings.aiVoice;
  c.instructions = buildInstructions();
  c.greeting = greeting ? greeting : "";
  c.outRate = g_settings.aiOutRate;
  c.toolsJson = kToolsJson;
  ui_face_caption(STR_AI_CONNECTING);
  ui_face_set_expr(EX_CONNECTING);
  Serial.printf("[ASSIST] starting session, heap=%u min=%u\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
  if (!VoiceSession::begin(c)) return false;
  s_sessionStartAt = millis();
  s_lastActivity = millis();
  return true;
}

void wakeUp(bool chime, const char *greeting) {
  WakeWord::setListening(false);      // the session owns the microphone now
  ui_screen_sleep(false);
  ui_show_face();
  ui_face_set_expr(EX_ATTENTIVE);
  if (chime) audio_prompt(AUDIO_PROMPT_WAKE);
  enterState(AS_WAKING);
  if (!startSession(greeting)) {
    s_failedAt = millis();
    enterState(AS_FAILED);
  }
}

// ---- tools ----
int argInt(const String &args, const char *key, int def, bool &found) {
  String pat = String("\"") + key + "\"";
  int i = args.indexOf(pat);
  found = false;
  if (i < 0) return def;
  i = args.indexOf(':', i);
  if (i < 0) return def;
  i++;
  while (i < (int)args.length() && (args[i] == ' ' || args[i] == '"')) i++;
  int j = i;
  if (j < (int)args.length() && (args[j] == '-' || args[j] == '+')) j++;
  while (j < (int)args.length() && isdigit((unsigned char)args[j])) j++;
  if (j == i) return def;
  found = true;
  return args.substring(i, j).toInt();
}

String argStr(const String &args, const char *key) {
  String pat = String("\"") + key + "\"";
  int i = args.indexOf(pat);
  if (i < 0) return "";
  i = args.indexOf(':', i); if (i < 0) return "";
  i = args.indexOf('"', i); if (i < 0) return "";
  int j = args.indexOf('"', i + 1); if (j < 0) return "";
  return args.substring(i + 1, j);
}

void runTool(const VoiceSession::Event &ev) {
  String out;
  if (ev.toolName == "set_volume") {
    bool hasLevel, hasDelta;
    int level = argInt(ev.toolArgs, "level", -1, hasLevel);
    int delta = argInt(ev.toolArgs, "delta", 0, hasDelta);
    int cur = H.getVolume ? H.getVolume() : 50;
    int target = hasLevel ? level : (hasDelta ? cur + delta : cur + 20);
    target = constrain(target, 0, 100);
    if (H.setVolume) H.setVolume(target);
    out = "{\"ok\":true,\"volume\":" + String(target) + "}";
    char b[32]; snprintf(b, sizeof(b), "%s %d%%", STR_VOLUME, target); ui_toast(b, 1200);
  } else if (ev.toolName == "set_brightness") {
    bool hasLevel, hasDelta;
    int level = argInt(ev.toolArgs, "level", -1, hasLevel);
    int delta = argInt(ev.toolArgs, "delta", 0, hasDelta);
    int cur = H.getBrightness ? H.getBrightness() : 80;
    int target = hasLevel ? level : (hasDelta ? cur + delta : cur + 20);
    target = constrain(target, 20, 100);
    if (H.setBrightness) H.setBrightness(target);
    out = "{\"ok\":true,\"brightness\":" + String(target) + "}";
    char b[32]; snprintf(b, sizeof(b), "%s %d%%", STR_BRIGHTNESS, target); ui_toast(b, 1200);
  } else if (ev.toolName == "call_front_desk") {
    String reason = argStr(ev.toolArgs, "reason");
    Serial.printf("[ASSIST] call_front_desk reason=\"%s\"\n", reason.c_str());
    s_pendingDial = true;
    s_pendingDialAt = millis() + 6000;   // dial after the model's closing sentence, or at the latest in 6 s
    ui_face_caption(STR_AI_CALLING);
    out = "{\"ok\":true,\"status\":\"dialing\"}";
  } else if (ev.toolName == "end_conversation") {
    out = "{\"ok\":true}";
    // let the goodbye finish playing, then sleep (handled in loop via response.done)
    s_lastActivity = millis() - (uint32_t)g_settings.aiIdleSec * 1000 + 4000;   // sleep in ~4 s
  } else {
    out = "{\"ok\":false,\"error\":\"unknown tool\"}";
  }
  VoiceSession::sendToolResult(ev.callId, out);
}

void doDial() {
  s_pendingDial = false;
  VoiceSession::flushPlayback();
  VoiceSession::end();
  if (audio_mode() == AUDIO_VOICE) audio_set_mode(AUDIO_IDLE);
  setRing(false);
  ui_face_caption("");
  s_resumeAfterCall = true;
  if (!(H.dialFrontDesk && H.dialFrontDesk())) {
    // could not dial: back to a fresh session so the guest is told
    s_resumeAfterCall = false;
    wakeUp(false, "告诉客人现在无法接通前台，请稍后再试或直接拨打房间电话。");
  }
}

}  // namespace

// ===========================================================================
void assistant_begin(const AssistantHooks &hooks) {
  H = hooks;
  s_state = AS_SLEEP;
  s_stateSince = millis();
}

AssistantState assistant_state() { return s_state; }

void assistant_on_wake_word() {
  if (s_state == AS_CALL) return;
  if (s_state == AS_SESSION || s_state == AS_WAKING) return;   // already up
  wakeUp(true, "请用一句话向客人问好并问需要什么帮助，不超过15个字。");
}

void assistant_on_user_wake() {
  if (s_state != AS_SLEEP && s_state != AS_FAILED) return;
  wakeUp(false, "请用一句话向客人问好并问需要什么帮助，不超过15个字。");
}

void assistant_on_call_started() {
  if (s_state == AS_SESSION || s_state == AS_WAKING) {
    VoiceSession::end();
    s_resumeAfterCall = true;
  } else if (s_state == AS_SLEEP) {
    ui_screen_sleep(false);
    s_resumeAfterCall = false;
  }
  setRing(false);
  ui_face_caption("");
  WakeWord::setListening(false);
  enterState(AS_CALL);
}

void assistant_on_call_ended() {
  if (s_state != AS_CALL) return;
  if (s_resumeAfterCall) {
    s_resumeAfterCall = false;
    wakeUp(false, "电话已经结束。请用一句话问客人还有没有其他需要帮助的，不超过15个字。");
  } else {
    // the phone woke a sleeping device (incoming call): stay awake briefly, then sleep
    WakeWord::setListening(true);
    enterState(AS_FAILED);
    s_failedAt = millis();
  }
}

void assistant_sleep_now() { if (s_state != AS_CALL) goSleep(); }
void assistant_touch_activity() { if (s_state == AS_SLEEP) s_stateSince = millis(); }

void assistant_loop() {
  const uint32_t now = millis();

  // session events
  VoiceSession::Event ev;
  while (VoiceSession::poll(ev)) {
    switch (ev.kind) {
      case VoiceSession::Event::CONNECTED:
        Serial.printf("[ASSIST] session connected in %lu ms\n", (unsigned long)(now - s_sessionStartAt));
        ui_face_caption("");
        ui_face_set_expr(EX_LISTENING);
        s_lastActivity = now;
        enterState(AS_SESSION);
        break;
      case VoiceSession::Event::FAILED:
        Serial.printf("[ASSIST] session failed: %s\n", ev.text.c_str());
        ui_face_caption(STR_AI_OFFLINE);
        ui_face_set_expr(EX_ERROR);
        if (audio_mode() == AUDIO_VOICE) audio_set_mode(AUDIO_IDLE);
        s_failedAt = now;
        enterState(AS_FAILED);
        break;
      case VoiceSession::Event::CLOSED:
        Serial.println("[ASSIST] session closed by server");
        if (s_state == AS_SESSION) {
          if (s_pendingDial) { doDial(); break; }
          ui_face_caption(STR_AI_OFFLINE);
          if (audio_mode() == AUDIO_VOICE) audio_set_mode(AUDIO_IDLE);
          s_failedAt = now;
          enterState(AS_FAILED);
        }
        break;
      case VoiceSession::Event::USER_SPEECH_START:
        s_lastActivity = now;
        setRing(false);
        ui_face_set_expr(EX_USER_SPEAKING);
        break;
      case VoiceSession::Event::USER_SPEECH_STOP:
        s_lastActivity = now;
        ui_face_set_expr(EX_THINKING);
        break;
      case VoiceSession::Event::TRANSCRIPT:
        ui_face_caption(ev.text.c_str());
        break;
      case VoiceSession::Event::TOOL_CALL:
        s_lastActivity = now;
        runTool(ev);
        break;
      case VoiceSession::Event::RESPONSE_DONE:
        if (s_pendingDial) s_pendingDialAt = now + 1500;   // let the last audio drain, then dial
        break;
      default: break;
    }
  }

  switch (s_state) {
    case AS_WAKING:
      // The audio path (AEC etc.) is switched on in AS_SESSION, i.e. after the
      // TLS handshake: internal RAM is tight and the two peaks must not overlap.
      if (now - s_stateSince > VOICE_CONNECT_TIMEOUT_MS + 5000) {   // belt and braces
        ui_face_caption(STR_AI_OFFLINE);
        s_failedAt = now;
        enterState(AS_FAILED);
      }
      break;

    case AS_SESSION: {
      if (audio_mode() == AUDIO_IDLE && !audio_prompt_busy()) {
        Serial.printf("[ASSIST] audio -> voice (heap %u)\n", (unsigned)ESP.getFreeHeap());
        audio_set_mode(AUDIO_VOICE);
      }
      const bool speaking = VoiceSession::playbackActive() || audio_spk_level() > 3;
      if (speaking) { s_lastActivity = now; s_lastSpeakingUi = now; }
      setRing(speaking);
      if (speaking) ui_face_set_expr(EX_SPEAKING);
      else if (now - s_lastSpeakingUi > 600 && now - VoiceSession::lastUserSpeechMs() > 1500) ui_face_set_expr(EX_LISTENING);
      if (now - VoiceSession::lastUserSpeechMs() < 1500) s_lastActivity = max(s_lastActivity, VoiceSession::lastUserSpeechMs());
      if (s_pendingDial && (int32_t)(now - s_pendingDialAt) >= 0 && !speaking) { doDial(); break; }
      if (now - s_lastActivity > (uint32_t)g_settings.aiIdleSec * 1000) {
        Serial.printf("[ASSIST] %u s without speech -> sleep\n", (unsigned)g_settings.aiIdleSec);
        goSleep();
      }
      break;
    }

    case AS_FAILED:
      if (audio_mode() == AUDIO_VOICE) audio_set_mode(AUDIO_IDLE);
      WakeWord::setListening(true);
      if (now - s_failedAt > 20000) goSleep();
      break;

    case AS_SLEEP:
      // Screen on but nobody talking (after boot, after a call, after settings):
      // dim out after the same idle period so the device ends up asleep.
      if (!ui_screen_asleep() && ui_screen() == UI_FACE && now - s_stateSince > (uint32_t)g_settings.aiIdleSec * 1000) {
        ui_face_set_expr(EX_SLEEPY);
        ui_screen_sleep(true);
      }
      break;
    case AS_CALL:
    default:
      break;
  }
}
