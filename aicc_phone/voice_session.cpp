#include "voice_session.h"
#include "config.h"
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <mbedtls/base64.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

namespace {

using namespace VoiceSession;

WebSocketsClient s_ws;
Config s_cfg;
volatile State s_state = VS_OFF;
TaskHandle_t s_task = nullptr;
QueueHandle_t s_evq = nullptr;       // Event* to the app
QueueHandle_t s_txq = nullptr;       // String* JSON to send from the ws task
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t s_ringLock = nullptr;

volatile bool s_sessionReady = false;
volatile bool s_responseActive = false;
volatile bool s_retryCreate = false;    // response.create refused while the user was speaking
volatile bool s_stopReq = false;
volatile uint32_t s_lastUser = 0, s_lastAi = 0;
String s_transcript;                 // current reply text (ws task only)
Stats s_st = {};

// ---- mic ring: 16 kHz int16, 1 s ----
const int MIC_CAP = 16000;
int16_t *s_mic = nullptr; int s_micHead = 0, s_micCount = 0;
// ---- play ring: int16 at outRate, 12 s ----
int s_playCap = 0;
int16_t *s_play = nullptr; int s_playHead = 0, s_playCount = 0;
volatile bool s_playing = false;
int s_prerollSamples = 0;

// fragment reassembly for frames larger than the lib's 15 KB frame limit
char *s_frag = nullptr; size_t s_fragLen = 0; const size_t FRAG_CAP = 96 * 1024;

void postEvent(Event::Kind k, const String &text = String(), const String &name = String(),
               const String &args = String(), const String &callId = String()) {
  if (!s_evq) return;
  Event *e = new Event();
  e->kind = k; e->text = text; e->toolName = name; e->toolArgs = args; e->callId = callId;
  if (xQueueSend(s_evq, &e, 0) != pdTRUE) delete e;
}

void queueSend(const String &json) {
  if (!s_txq) return;
  String *s = new String(json);
  if (xQueueSend(s_txq, &s, pdMS_TO_TICKS(50)) != pdTRUE) delete s;
}

// ---------------------------------------------------------------------------
// tiny JSON helpers (flat string fields only; values may contain \uXXXX)
// ---------------------------------------------------------------------------
void appendUtf8(String &o, uint32_t cp) {
  if (cp < 0x80) o += (char)cp;
  else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
  else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
}

// Finds "key":"value" and returns the unescaped value. `from` lets the caller skip nested objects.
bool jsonString(const char *js, const char *key, String &out, size_t maxLen = 0) {
  String pat = String("\"") + key + "\"";
  const char *p = strstr(js, pat.c_str());
  while (p) {
    const char *q = p + pat.length();
    while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
    if (*q == ':') {
      q++;
      while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
      if (*q != '"') return false;
      q++;
      out = "";
      while (*q && *q != '"') {
        if (*q == '\\') {
          q++;
          switch (*q) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': break;
            case 'b': case 'f': break;
            case 'u': {
              uint32_t cp = strtoul(String(q + 1).substring(0, 4).c_str(), nullptr, 16);
              q += 4;
              if (cp >= 0xD800 && cp <= 0xDBFF && q[1] == '\\' && q[2] == 'u') {
                uint32_t lo = strtoul(String(q + 3).substring(0, 4).c_str(), nullptr, 16);
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                q += 6;
              }
              appendUtf8(out, cp);
              break;
            }
            default: out += *q; break;
          }
          q++;
        } else {
          out += *q++;
        }
        if (maxLen && out.length() >= maxLen) break;
      }
      return true;
    }
    p = strstr(q, pat.c_str());
  }
  return false;
}

String jsonEscape(const String &s) {
  String o; o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if (c == '\r') continue;
    else if ((uint8_t)c < 0x20) continue;
    else o += c;
  }
  return o;
}

// ---------------------------------------------------------------------------
// rings
// ---------------------------------------------------------------------------
void ringsReset() {
  xSemaphoreTake(s_ringLock, portMAX_DELAY);
  s_micHead = s_micCount = 0;
  s_playHead = s_playCount = 0;
  s_playing = false;
  xSemaphoreGive(s_ringLock);
}

void playPush(const int16_t *pcm, int n) {
  xSemaphoreTake(s_ringLock, portMAX_DELAY);
  for (int i = 0; i < n; i++) {
    if (s_playCount == s_playCap) { s_playHead = (s_playHead + 1) % s_playCap; s_playCount--; s_st.playDrops++; }
    s_play[(s_playHead + s_playCount) % s_playCap] = pcm[i];
    s_playCount++;
  }
  xSemaphoreGive(s_ringLock);
}

// ---------------------------------------------------------------------------
// protocol
// ---------------------------------------------------------------------------
String sessionUpdateJson() {
  String j;
  j.reserve(1200 + s_cfg.instructions.length() + s_cfg.toolsJson.length());
  j += "{\"type\":\"session.update\",\"session\":{";
  j += "\"modalities\":[\"text\",\"audio\"],";
  if (s_cfg.voice.length()) j += "\"voice\":\"" + jsonEscape(s_cfg.voice) + "\",";
  j += "\"instructions\":\"" + jsonEscape(s_cfg.instructions) + "\",";
  j += "\"input_audio_format\":\"pcm16\",\"output_audio_format\":\"pcm16\",";
  j += "\"turn_detection\":{\"type\":\"" VOICE_TURN_DETECTION "\"},";
  if (s_cfg.toolsJson.length()) j += "\"tools\":" + s_cfg.toolsJson + ",\"tool_choice\":\"auto\",";
  if (j.endsWith(",")) j.remove(j.length() - 1);
  j += "}}";
  return j;
}

void handleAudioDelta(const char *js) {
  // locate "delta":"...." without copying through String (payload can be 10+ KB)
  const char *p = strstr(js, "\"delta\"");
  if (!p) return;
  p = strchr(p + 7, '"'); if (!p) return;     // opening quote of value
  p++;
  const char *e = strchr(p, '"'); if (!e) return;
  size_t b64len = e - p;
  size_t need = b64len / 4 * 3 + 3;
  static uint8_t *buf = nullptr; static size_t bufCap = 0;
  if (need > bufCap) { free(buf); buf = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); bufCap = buf ? need : 0; }
  if (!buf) return;
  size_t olen = 0;
  if (mbedtls_base64_decode(buf, bufCap, &olen, (const uint8_t *)p, b64len) != 0) return;
  playPush((const int16_t *)buf, olen / 2);
  s_st.rxAudioBytes += olen;
  s_lastAi = millis();
}

void handleMessage(const char *js, size_t len) {
  s_st.events++;
  String type;
  if (!jsonString(js, "type", type, 64)) return;

  if (type == "response.audio.delta") { handleAudioDelta(js); return; }

  if (type == "response.audio_transcript.delta") {
    String d; if (jsonString(js, "delta", d)) { s_transcript += d; postEvent(Event::TRANSCRIPT, s_transcript); }
    return;
  }
  if (type == "response.audio_transcript.done") {
    String t; if (jsonString(js, "transcript", t)) { s_transcript = t; postEvent(Event::TRANSCRIPT, s_transcript); }
    return;
  }
  if (type == "session.created") {
    queueSend(sessionUpdateJson());
    return;
  }
  if (type == "session.updated") {
    s_sessionReady = true;
    s_state = VS_READY;
    Serial.printf("[VOICE] session ready (%u bytes) heap=%u: %.200s\n", (unsigned)len, (unsigned)ESP.getFreeHeap(), js);
    postEvent(Event::CONNECTED);
    if (s_cfg.greeting.length()) requestResponse(s_cfg.greeting);
    return;
  }
  if (type == "input_audio_buffer.speech_started") {
    s_lastUser = millis();
    // barge-in: drop whatever the model already streamed
    flushPlayback();
    if (s_responseActive) queueSend("{\"type\":\"response.cancel\"}");
    postEvent(Event::USER_SPEECH_START);
    return;
  }
  if (type == "input_audio_buffer.speech_stopped") {
    s_lastUser = millis();
    postEvent(Event::USER_SPEECH_STOP);
    if (s_retryCreate) { s_retryCreate = false; queueSend("{\"type\":\"response.create\"}"); }
    return;
  }
  if (type == "response.created") { s_responseActive = true; s_transcript = ""; return; }
  if (type == "response.done") {
    s_responseActive = false;
    String st; jsonString(js, "status", st, 32);
    Serial.printf("[VOICE] response.done status=%s\n", st.c_str());
    postEvent(Event::RESPONSE_DONE, st);
    return;
  }
  if (type == "response.function_call_arguments.done") {
    String name, args, cid;
    jsonString(js, "name", name, 64);
    jsonString(js, "arguments", args, 512);
    jsonString(js, "call_id", cid, 96);
    Serial.printf("[VOICE] tool call %s(%s) id=%s\n", name.c_str(), args.c_str(), cid.c_str());
    postEvent(Event::TOOL_CALL, String(), name, args, cid);
    return;
  }
  if (type == "error") {
    String msg; jsonString(js, "message", msg, 200);
    Serial.printf("[VOICE] server error: %.400s\n", js);
    if (msg.indexOf("while user is speaking") >= 0) { s_retryCreate = true; return; }
    if (!s_sessionReady) { s_state = VS_ERROR; postEvent(Event::FAILED, msg.length() ? msg : String("server error")); }
    return;
  }
  // everything else: log the type only
  if (type.startsWith("response.audio") || type.startsWith("conversation.item") || type.startsWith("rate_limits") ||
      type.startsWith("response.output_item") || type.startsWith("response.content_part") || type.startsWith("input_audio_buffer")) return;
  Serial.printf("[VOICE] event %s\n", type.c_str());
}

void onWsEvent(WStype_t t, uint8_t *payload, size_t len) {
  switch (t) {
    case WStype_CONNECTED:
      Serial.printf("[VOICE] ws connected (%s) heap=%u\n", (const char *)payload, (unsigned)ESP.getFreeHeap());
      break;
    case WStype_DISCONNECTED:
      Serial.printf("[VOICE] ws disconnected (state %d)\n", (int)s_state);
      if (s_state == VS_CONNECTING) { s_state = VS_ERROR; postEvent(Event::FAILED, "connect failed"); }
      else if (s_state == VS_READY) { s_state = VS_ERROR; postEvent(Event::CLOSED); }
      break;
    case WStype_TEXT:
      s_st.bytesIn += len;
      handleMessage((const char *)payload, len);
      break;
    case WStype_FRAGMENT_TEXT_START:
      s_fragLen = 0;
      // fallthrough
    case WStype_FRAGMENT:
      if (s_frag && s_fragLen + len < FRAG_CAP) { memcpy(s_frag + s_fragLen, payload, len); s_fragLen += len; }
      break;
    case WStype_FRAGMENT_FIN:
      if (s_frag && s_fragLen + len < FRAG_CAP) {
        memcpy(s_frag + s_fragLen, payload, len); s_fragLen += len; s_frag[s_fragLen] = 0;
        s_st.bytesIn += s_fragLen;
        handleMessage(s_frag, s_fragLen);
      }
      s_fragLen = 0;
      break;
    case WStype_ERROR:
      Serial.printf("[VOICE] ws error: %.*s\n", (int)len, (const char *)payload);
      break;
    default: break;
  }
}

// ---------------------------------------------------------------------------
// task
// ---------------------------------------------------------------------------
void sendMicChunk() {
  // 100 ms of 16 kHz PCM per append
  const int N = 1600;
  static int16_t *pcm = (int16_t *)heap_caps_malloc(N * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!pcm) return;
  xSemaphoreTake(s_ringLock, portMAX_DELAY);
  if (s_micCount < N) { xSemaphoreGive(s_ringLock); return; }
  for (int i = 0; i < N; i++) { pcm[i] = s_mic[s_micHead]; s_micHead = (s_micHead + 1) % MIC_CAP; }
  s_micCount -= N;
  xSemaphoreGive(s_ringLock);
  const size_t B64CAP = N * 2 / 3 * 4 + 8, MSGCAP = B64CAP + 64;
  static char *b64 = (char *)heap_caps_malloc(B64CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  static char *msg = (char *)heap_caps_malloc(MSGCAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!b64 || !msg) return;
  size_t olen = 0;
  if (mbedtls_base64_encode((uint8_t *)b64, B64CAP, &olen, (const uint8_t *)pcm, N * 2) != 0) return;
  int n = snprintf(msg, MSGCAP, "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%.*s\"}", (int)olen, b64);
  s_ws.sendTXT(msg, n);
  s_st.txChunks++;
}

void voiceTask(void *) {
  String url = "/api-ws/v1/realtime?model=" + s_cfg.model;
  String hdr = "Authorization: Bearer " + s_cfg.apiKey;
  s_ws.setExtraHeaders(hdr.c_str());
  s_ws.setReconnectInterval(3000);
  s_ws.enableHeartbeat(15000, 5000, 2);
  s_ws.onEvent(onWsEvent);
  Serial.printf("[VOICE] connecting wss://%s%s heap=%u\n", s_cfg.host.c_str(), url.c_str(), (unsigned)ESP.getFreeHeap());
  s_ws.beginSSL(s_cfg.host.c_str(), 443, url.c_str());   // no CA pinned: TLS encrypts, identity is not verified

  uint32_t started = millis();
  for (;;) {
    if (s_stopReq) break;
    s_ws.loop();
    // outgoing JSON from the app
    String *js = nullptr;
    while (s_txq && xQueueReceive(s_txq, &js, 0) == pdTRUE) {
      if (s_ws.isConnected()) s_ws.sendTXT(js->c_str(), js->length());
      delete js;
    }
    if (s_sessionReady && s_ws.isConnected()) {
      sendMicChunk();
    } else if (s_state == VS_CONNECTING && millis() - started > VOICE_CONNECT_TIMEOUT_MS) {
      Serial.println("[VOICE] connect timeout");
      s_state = VS_ERROR;
      postEvent(Event::FAILED, "connect timeout");
      break;
    } else if (s_state == VS_ERROR) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  s_ws.disconnect();
  // drain tx queue
  String *js = nullptr;
  while (s_txq && xQueueReceive(s_txq, &js, 0) == pdTRUE) delete js;
  s_sessionReady = false;
  s_responseActive = false;
  if (s_state != VS_ERROR) s_state = VS_OFF;
  Serial.printf("[VOICE] task exit state=%d heap=%u\n", (int)s_state, (unsigned)ESP.getFreeHeap());
  s_task = nullptr;
  vTaskDelete(nullptr);
}

}  // namespace

// ===========================================================================
bool VoiceSession::begin(const Config &cfg) {
  if (s_task) end();
  if (!s_ringLock) s_ringLock = xSemaphoreCreateMutex();
  if (!s_evq) s_evq = xQueueCreate(16, sizeof(Event *));
  if (!s_txq) s_txq = xQueueCreate(8, sizeof(String *));
  if (!s_mic) s_mic = (int16_t *)heap_caps_malloc(MIC_CAP * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_frag) s_frag = (char *)heap_caps_malloc(FRAG_CAP + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  s_cfg = cfg;
  if (s_cfg.outRate < 8000 || s_cfg.outRate > 48000) s_cfg.outRate = 24000;
  int cap = (int)s_cfg.outRate * 12;
  if (cap != s_playCap) { free(s_play); s_play = (int16_t *)heap_caps_malloc(cap * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); s_playCap = s_play ? cap : 0; }
  s_prerollSamples = (int)s_cfg.outRate * VOICE_PREROLL_MS / 1000;
  if (!s_mic || !s_play || !s_frag) { Serial.println("[VOICE] buffer alloc failed"); return false; }
  if (!cfg.apiKey.length()) { Serial.println("[VOICE] no API key configured"); s_state = VS_ERROR; postEvent(Event::FAILED, "no api key"); return false; }
  ringsReset();
  memset(&s_st, 0, sizeof(s_st));
  s_transcript = "";
  s_sessionReady = false;
  s_responseActive = false;
  s_retryCreate = false;
  s_stopReq = false;
  s_lastUser = s_lastAi = millis();
  s_state = VS_CONNECTING;
  // Core 0 next to WiFi; priority between the audio task (12) and loop().
  if (xTaskCreatePinnedToCore(voiceTask, "voice", 12288, nullptr, 6, &s_task, 0) != pdPASS) {
    s_state = VS_ERROR; s_task = nullptr; return false;
  }
  return true;
}

void VoiceSession::end() {
  if (s_task) {
    s_stopReq = true;
    for (int i = 0; i < 100 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(10));
  }
  s_state = VS_OFF;
  s_sessionReady = false;
  s_responseActive = false;
  if (s_ringLock) ringsReset();
  Event *e = nullptr;
  while (s_evq && xQueueReceive(s_evq, &e, 0) == pdTRUE) delete e;
}

VoiceSession::State VoiceSession::state() { return s_state; }
bool VoiceSession::connected() { return s_state == VS_READY; }

bool VoiceSession::poll(Event &ev) {
  Event *e = nullptr;
  if (!s_evq || xQueueReceive(s_evq, &e, 0) != pdTRUE) return false;
  ev = *e;
  delete e;
  return true;
}

void VoiceSession::sendToolResult(const String &callId, const String &outputJson) {
  String j = "{\"type\":\"conversation.item.create\",\"item\":{\"type\":\"function_call_output\",\"call_id\":\"" +
             jsonEscape(callId) + "\",\"output\":\"" + jsonEscape(outputJson) + "\"}}";
  queueSend(j);
  queueSend("{\"type\":\"response.create\"}");
}

void VoiceSession::requestResponse(const String &instructions) {
  // Verified against the live API: response.create on a conversation without a
  // user message is rejected, so seed a (text) user turn first.
  queueSend("{\"type\":\"conversation.item.create\",\"item\":{\"type\":\"message\",\"role\":\"user\","
            "\"content\":[{\"type\":\"input_text\",\"text\":\"（客人刚唤醒了设备）\"}]}}");
  String j = "{\"type\":\"response.create\",\"response\":{\"instructions\":\"" + jsonEscape(instructions) + "\"}}";
  queueSend(j);
}

void VoiceSession::flushPlayback() {
  if (!s_ringLock) return;
  xSemaphoreTake(s_ringLock, portMAX_DELAY);
  s_playHead = s_playCount = 0;
  s_playing = false;
  xSemaphoreGive(s_ringLock);
}

void VoiceSession::pushMic(const int16_t *pcm, int n) {
  if (!s_mic || !s_sessionReady) return;
  xSemaphoreTake(s_ringLock, portMAX_DELAY);
  for (int i = 0; i < n; i++) {
    if (s_micCount == MIC_CAP) { s_micHead = (s_micHead + 1) % MIC_CAP; s_micCount--; }
    s_mic[(s_micHead + s_micCount) % MIC_CAP] = pcm[i];
    s_micCount++;
  }
  xSemaphoreGive(s_ringLock);
}

int VoiceSession::popPlay(int16_t *out, int n) {
  if (!s_play) return 0;
  xSemaphoreTake(s_ringLock, portMAX_DELAY);
  if (!s_playing) {
    if (s_playCount >= s_prerollSamples) s_playing = true;
    else { xSemaphoreGive(s_ringLock); return 0; }
  }
  if (s_playCount < n) {
    // underrun: play what is left padded with silence, then wait for preroll again
    int got = s_playCount;
    for (int i = 0; i < got; i++) { out[i] = s_play[s_playHead]; s_playHead = (s_playHead + 1) % s_playCap; }
    for (int i = got; i < n; i++) out[i] = 0;
    s_playCount = 0;
    s_playing = false;
    if (got == 0) { xSemaphoreGive(s_ringLock); return 0; }
    s_st.playUnderruns++;
    xSemaphoreGive(s_ringLock);
    return n;
  }
  for (int i = 0; i < n; i++) { out[i] = s_play[s_playHead]; s_playHead = (s_playHead + 1) % s_playCap; }
  s_playCount -= n;
  xSemaphoreGive(s_ringLock);
  return n;
}

bool VoiceSession::playbackActive() { return s_playing || s_playCount >= s_prerollSamples; }
uint32_t VoiceSession::playbackRate() { return s_cfg.outRate; }
uint32_t VoiceSession::lastUserSpeechMs() { return s_lastUser; }
uint32_t VoiceSession::lastAiSpeechMs() { return s_lastAi; }

VoiceSession::Stats VoiceSession::stats() {
  Stats s = s_st;
  s.playDepthMs = s_playCap ? (int)((int64_t)s_playCount * 1000 / s_cfg.outRate) : 0;
  return s;
}
