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
bool s_sleepAfterReply = false;                     // end_conversation: sleep once the goodbye has played
uint32_t s_failedAt = 0;
bool s_resumeAfterCall = false;    // a session was running when the call started
bool s_pendingDial = false;        // call_front_desk accepted: dial after the model finishes its sentence
uint32_t s_pendingDialAt = 0;
bool s_greetAfterCall = false;
uint32_t s_lastSpeakingUi = 0;

uint8_t s_stallRetries = 0;    // uplink stall -> session restarts in a row
uint8_t s_failedRetries = 0;   // connect failures -> immediate retries

const char *kToolsJson =
  "["
   "{\"type\":\"function\",\"name\":\"set_volume\",\"description\":\"调节这台话机扬声器的播放音量。只要客人的话涉及声音大小就必须立即调用，不要只口头答应。触发例子：大声点、小声一点、太吵了、声音太大、听不清、你说话轻一点、再响一点、静音、别出声、音量调到60、调成一半、最大声、小一点点。注意：这调的是本机喇叭音量，不是让你改变说话语气。\","
     "\"parameters\":{\"type\":\"object\",\"properties\":{"
       "\"level\":{\"type\":\"integer\",\"description\":\"目标音量绝对值 0-100。客人说出具体数值或“最大/最小/一半/静音”时用它（静音=0，最大=100，一半=50）。与 delta 二选一\"},"
       "\"delta\":{\"type\":\"integer\",\"description\":\"相对调整量，客人只说大点/小点而没给数值时用它，调大用正数调小用负数，一般 ±20，“大很多/小很多”用 ±40。与 level 二选一\"}}}},"
   "{\"type\":\"function\",\"name\":\"set_brightness\",\"description\":\"调节这台话机屏幕的背光亮度。只要客人的话涉及屏幕明暗就必须立即调用，不要只口头答应。触发例子：太亮了、太暗了、亮一点、暗一点、刺眼、晚上调暗点、屏幕看不清、调亮些、亮度调到80、调成一半、最亮、最暗。\","
     "\"parameters\":{\"type\":\"object\",\"properties\":{"
       "\"level\":{\"type\":\"integer\",\"description\":\"目标亮度绝对值 20-100。客人说出具体数值或“最亮/最暗/一半”时用它（最暗=20，最亮=100，一半=60）。与 delta 二选一\"},"
       "\"delta\":{\"type\":\"integer\",\"description\":\"相对调整量，客人只说亮点/暗点而没给数值时用它，调亮用正数调暗用负数，一般 ±20。与 level 二选一\"}}}},"
   "{\"type\":\"function\",\"name\":\"call_front_desk\",\"description\":\"用这台话机拨打酒店前台，接听的是酒店客服 AI，负责酒店自己的业务与政策。遇到下列情况就调用：客人问酒店的早餐时间/退房时间/房型房价/账单发票等政策；客人要送东西（毛巾、水、牙刷等）、要维修（空调、灯、马桶等）、要投诉、要叫车、要打扫、要加床；客人明确说要打给前台/打电话给前台/接前台/拨前台/帮我打前台/给前台打电话/找前台/找人工/转真人/叫服务员——只要客人表达了联系前台的意思就立即调用，不要只口头答应。普通知识、闲聊、算数、翻译等绝对不要调用，由你自己回答。调用前先用一句话告诉客人“马上为您接通前台”，然后停止说话。\","
     "\"parameters\":{\"type\":\"object\",\"properties\":{\"reason\":{\"type\":\"string\",\"description\":\"客人的诉求，一句话概括，例如“客人要两瓶水”\"}}}},"
   "{\"type\":\"function\",\"name\":\"end_conversation\",\"description\":\"结束本次对话并让设备休眠。客人明确表示不再需要帮助时调用，例子：再见、拜拜、没事了、就这样、不用了、谢谢不用了、可以了、好了没别的事了、挂了吧。调用前先简短道个别（如“好的，有需要再叫我”）再调用。\","
     "\"parameters\":{\"type\":\"object\",\"properties\":{}}}"
  "]";

String buildInstructions() {
  String s;
  s.reserve(1400 + g_settings.aiExtra.length());
  s += "你是放在酒店客房里的智能语音助手“小安”，就在客人手边的圆形小话机里。";
  s += "你知识面很广，能像一位见多识广的朋友那样聊天：生活常识与实用技巧、饮食与健康常识、历史、经济与金融常识、"
       "人文与艺术、天文与地理、自然科学与技术、旅行见闻、语言学习，也能做简单计算、翻译、润色句子、出主意、讲个段子。";
  s += "规则：";
  s += "1）口语化、简短，一般一到三句话；不要分点列举、不要念稿子，客人追问时再展开讲。";
  s += "2）知识类、闲聊类的问题由你自己直接回答，不要推给前台，也不要说“请联系前台”。";
  s += "3）确实不知道或不确定的，坦率说不太清楚，不要编造年份、数据、人名。";
  s += "4）你能直接操作这台话机：调音量用 set_volume，调屏幕亮度用 set_brightness，找前台用 call_front_desk，结束对话用 end_conversation。"
       "只要客人的意思落在某个工具上，第一时间调用那个工具，再用一句话确认结果（如“音量调到60了”）。"
       "绝对不要只嘴上答应“好的”“已为您调整”却不调用工具——没调用工具就等于什么都没做。拿不准数值时按工具说明的默认值调。";
  s += "5）涉及酒店自己的服务与政策，或客人直接要求联系前台时，调用 call_front_desk：早餐与退房时间、房型房价、账单发票、送物、维修、投诉、叫车、"
       "以及客人说“打给前台/接前台/找前台/找人工/叫服务员”等任何要联系前台的话。电话那头是酒店客服 AI，这些事由它回答。接通前告诉客人正在为他接通前台，然后不要再说话。";
  s += "6）客人说再见、没事了、谢谢不用了，先简短道别，再调用 end_conversation。";
  s += "7）用客人使用的语言回答，默认普通话。";
  if (g_settings.aiExtra.length()) { s += "8）本机已知的一些环境信息，需要时自然使用："; s += g_settings.aiExtra; }
  return s;
}

void enterState(AssistantState st) {
  if (s_state == st) return;
  Serial.printf("[ASSIST] %d -> %d\n", (int)s_state, (int)st);
  s_state = st;
  s_stateSince = millis();
}

void clearCaption() { ui_face_caption(""); }

void goSleep() {
  VoiceSession::end();
  if (audio_mode() == AUDIO_VOICE) audio_set_mode(AUDIO_IDLE);
  clearCaption();
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
  s_sleepAfterReply = false;
  return true;
}

void wakeUp(bool chime, const char *greeting) {
  WakeWord::setListening(true);       // keep WakeNet on so "你好小安" can barge in mid-reply
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
    // Sleep once the goodbye has actually played (AS_SESSION handles it), so a
    // long closing sentence is never cut off and a short one does not linger.
    s_sleepAfterReply = true;
  } else {
    out = "{\"ok\":false,\"error\":\"unknown tool\"}";
  }
  VoiceSession::sendToolResult(ev.callId, out);
}

void doDial() {
  s_pendingDial = false;
  s_sleepAfterReply = false;
  VoiceSession::flushPlayback();
  VoiceSession::end();
  if (audio_mode() == AUDIO_VOICE) audio_set_mode(AUDIO_IDLE);
  clearCaption();
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
  if (s_state == AS_CALL) return;     // during a SIP call: ignore (also not fed)
  if (s_state == AS_SESSION) {
    // Mid-session: "你好小安" interrupts the reply, but only while the AI is
    // actually speaking; if it is already listening there is nothing to stop.
    if (VoiceSession::playbackActive() || audio_spk_level() > 3) {
      Serial.println("[ASSIST] wake-word barge-in");
      VoiceSession::bargeIn();
      s_lastActivity = millis();
      ui_face_set_expr(EX_USER_SPEAKING);
    }
    return;
  }
  if (s_state == AS_WAKING) return;   // connecting
  s_failedRetries = 0;
  wakeUp(true, "请用一句话向客人问好，并问他想聊点什么或需要什么帮忙，不超过15个字。");
}

void assistant_on_user_wake() {
  if (s_state != AS_SLEEP && s_state != AS_FAILED) return;
  wakeUp(false, "请用一句话向客人问好，并问他想聊点什么或需要什么帮忙，不超过15个字。");
}

void assistant_on_call_started() {
  if (s_state == AS_SESSION || s_state == AS_WAKING) {
    VoiceSession::end();
    s_resumeAfterCall = true;
  } else if (s_state == AS_SLEEP) {
    ui_screen_sleep(false);
    s_resumeAfterCall = false;
  }
  clearCaption();
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
        s_failedRetries = 0;
        clearCaption();
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
        ui_face_set_expr(EX_USER_SPEAKING);
        break;
      case VoiceSession::Event::USER_SPEECH_STOP:
        s_lastActivity = now;
        ui_face_set_expr(EX_THINKING);
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
      // A stalled uplink keeps isConnected() true while every append is refused,
      // so the device goes deaf for tens of seconds and the model only ever
      // hears the last 600 ms of the ring. Restart the session instead.
      if (now - s_sessionStartAt > 60000) s_stallRetries = 0;
      const uint32_t stallMs = VoiceSession::uplinkStallMs();
      if (stallMs > 6000) {
        Serial.printf("[ASSIST] uplink stalled %lums -> %s (%u)\n", (unsigned long)stallMs,
                      s_stallRetries < 2 ? "reconnect" : "give up", (unsigned)s_stallRetries);
        VoiceSession::end();
        if (audio_mode() == AUDIO_VOICE) audio_set_mode(AUDIO_IDLE);
        if (s_stallRetries < 2) {
          s_stallRetries++;
          wakeUp(false, "网络刚才断了一下，请用一句话向客人说明，并问他还需要什么帮助。");
        } else {
          s_stallRetries = 0;
          s_failedAt = now;
          enterState(AS_FAILED);
          ui_face_caption(STR_AI_OFFLINE);
        }
        break;
      }
      const bool speaking = VoiceSession::playbackActive() || audio_spk_level() > 3;
      // Interruptions (xiaozhi-esp32 realtime mode): the microphone keeps streaming
      // through the AEC while the reply plays and the *server* decides when the
      // guest is talking over it. Its input_audio_buffer.speech_started flushes
      // local playback and cancels the response (voice_session.cpp); nothing is
      // cut locally. The local VAD only colours the face.
      if (speaking && audio_vad_state()) ui_face_set_expr(EX_USER_SPEAKING);
      if (speaking) { s_lastActivity = now; s_lastSpeakingUi = now; }
      if (speaking) ui_face_set_expr(EX_SPEAKING);
      else if (now - s_lastSpeakingUi > 600 && now - VoiceSession::lastUserSpeechMs() > 1500) ui_face_set_expr(EX_LISTENING);
      if (now - VoiceSession::lastUserSpeechMs() < 1500) s_lastActivity = max(s_lastActivity, VoiceSession::lastUserSpeechMs());
      if (s_pendingDial && (int32_t)(now - s_pendingDialAt) >= 0 && !speaking) { doDial(); break; }
      if (s_sleepAfterReply && !speaking && now - s_lastActivity > 500) {
        Serial.println("[ASSIST] end_conversation -> sleep");
        s_sleepAfterReply = false;
        goSleep();
        break;
      }
      if (now - s_lastActivity > (uint32_t)g_settings.aiIdleSec * 1000) {
        Serial.printf("[ASSIST] %u s without speech -> sleep\n", (unsigned)g_settings.aiIdleSec);
        goSleep();
      }
      break;
    }

    case AS_FAILED:
      if (audio_mode() == AUDIO_VOICE) audio_set_mode(AUDIO_IDLE);
      WakeWord::setListening(true);
      // A single failed TLS handshake should not end the conversation: the link
      // to DashScope is flaky on weak Wi-Fi. Retry twice before giving up.
      if (s_failedRetries < 2 && now - s_failedAt > 1200) {
        s_failedRetries++;
        Serial.printf("[ASSIST] connect failed -> retry %u\n", (unsigned)s_failedRetries);
        wakeUp(false, "刚才没连上，请用一句话向客人问好并问他想聊点什么。");
        break;
      }
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
