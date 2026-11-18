#include "voice_session.h"
#include "config.h"
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <mbedtls/base64.h>
#include <esp_heap_caps.h>
#include <sdkconfig.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
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
volatile bool s_taskDone = false;
void reapTask() {
  if (s_task && s_taskDone) { vTaskDeleteWithCaps(s_task); s_task = nullptr; s_taskDone = false; }
}
volatile uint32_t s_lastUser = 0;
uint32_t s_lastSendMs = 0;   // last successful input_audio_buffer.append
uint32_t s_beginMs = 0;
Stats s_st = {};

// ---- mic ring: 16 kHz int16, 1 s (oldest dropped beyond 600 ms of backlog) ----
const int MIC_CAP = 16000 * 1;
const int MIC_MAX_BACKLOG = 16000 * 3 / 5;   // 600 ms
int16_t *s_mic = nullptr; int s_micHead = 0, s_micCount = 0;
// ---- play ring: int16 at outRate, 4 s ----
int s_playCap = 0;
int16_t *s_play = nullptr; int s_playHead = 0, s_playCount = 0;
volatile bool s_playing = false;
int s_prerollSamples = 0;

// barge-in: after the server reports user speech, audio still in flight for the
// cancelled response must not be played. Cleared when the next response starts.
volatile bool s_dropAudio = false;

// fragment reassembly for frames larger than the lib's 15 KB frame limit
char *s_frag = nullptr; size_t s_fragLen = 0; bool s_fragOverflow = false;
const size_t FRAG_CAP = 64 * 1024;

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

// Rings are shared by the ws task and the audio task. The whole copy runs
// inside the lock: a copy is at most ~1.3 KB, while releasing the lock between
// the index snapshot and the commit let one side overwrite the other's update.
void playPush(const int16_t *pcm, int n) {
  if (!s_play || n <= 0) return;
  xSemaphoreTake(s_ringLock, portMAX_DELAY);
  int head = s_playHead, count = s_playCount;
  if (count + n > s_playCap) {                      // drop oldest
    int drop = count + n - s_playCap;
    head = (head + drop) % s_playCap; count -= drop; s_st.playDrops += drop;
  }
  int tail = (head + count) % s_playCap;
  int first = min(n, s_playCap - tail);
  memcpy(&s_play[tail], pcm, first * sizeof(int16_t));
  if (n > first) memcpy(&s_play[0], pcm + first, (n - first) * sizeof(int16_t));
  s_playHead = head;
  s_playCount = count + n;
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
  // 联网搜索: the model decides per turn whether to search; enable_source adds
  // citation metadata to the result. Works alongside function calling on
  // qwen-audio-3.1-realtime-flash.
  if (s_cfg.enableSearch) j += "\"enable_search\":true,\"search_options\":{\"enable_source\":true},";
  if (j.endsWith(",")) j.remove(j.length() - 1);
  j += "}}";
  return j;
}

void handleAudioDelta(const char *js) {
  // Barge-in: the cancelled response may still have frames in flight. Drop them
  // until the next response.created, otherwise 100-400 ms of stale AI speech
  // plays after the guest has interrupted.
  if (s_dropAudio) { s_st.playDrops++; return; }
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
}

void handleMessage(const char *js, size_t len) {
  s_st.events++;
  String type;
  if (!jsonString(js, "type", type, 64)) return;

  if (type == "response.audio.delta") { handleAudioDelta(js); return; }
  if (type != "response.audio_transcript.delta")
    Serial.printf("[VOICE] << %s (%u B) int=%u\n", type.c_str(), (unsigned)len, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  if (type == "conversation.item.input_audio_transcription.completed") {
    String t; if (jsonString(js, "transcript", t, 160)) Serial.printf("[VOICE] user said: %s\n", t.c_str());
    return;
  }
  // The reply text is not shown on screen any more (labelling it forced a full
  // CJK re-layout several times a second); the deltas are simply ignored.
  if (type == "response.audio_transcript.delta" || type == "response.audio_transcript.done") return;
  if (type == "session.created") {
    queueSend(sessionUpdateJson());
    return;
  }
  if (type == "session.updated") {
    s_sessionReady = true;
    s_state = VS_READY;
    // The uplink watchdog counts from the moment the session can actually send;
    // counting from begin() makes the TLS handshake itself look like a stall.
    s_lastSendMs = millis();
    Serial.printf("[VOICE] session ready (%u bytes) heap=%u: %.200s\n", (unsigned)len, (unsigned)ESP.getFreeHeap(), js);
    postEvent(Event::CONNECTED);
    if (s_cfg.greeting.length()) requestResponse(s_cfg.greeting);
    return;
  }
  if (type == "input_audio_buffer.speech_started") {
    s_lastUser = millis();
    // barge-in: drop what is buffered now and anything still in flight for the
    // cancelled response (cleared again on the next response.created).
    s_dropAudio = true;
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
  if (type == "response.created") { s_responseActive = true; s_dropAudio = false; return; }
  if (type == "response.done") {
    s_responseActive = false;
    s_dropAudio = false;      // the cancelled response is over; nothing to mute for it
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
    if (msg.indexOf("no active response") >= 0) {
      // Barge-in raced with the end of the reply: the server had already finished
      // streaming it (audio is generated faster than real time). Harmless.
      Serial.println("[VOICE] response.cancel: reply had already completed");
      return;
    }
    Serial.printf("[VOICE] server error: %.400s\n", js);
    if (msg.indexOf("while user is speaking") >= 0) { s_retryCreate = true; return; }
    if (!s_sessionReady) { s_state = VS_ERROR; postEvent(Event::FAILED, msg.length() ? msg : String("server error")); }
    return;
  }
  // everything else: log the type only
  if (type.startsWith("response.audio") || type.startsWith("conversation.item") || type.startsWith("rate_limits") ||
      type.startsWith("response.output_item") || type.startsWith("response.content_part") || type.startsWith("input_audio_buffer") ||
      type.startsWith("response.function_call_arguments.delta")) return;
  Serial.printf("[VOICE] event %s\n", type.c_str());
}

void onWsEvent(WStype_t t, uint8_t *payload, size_t len) {
  switch (t) {
    case WStype_CONNECTED:
      Serial.printf("[VOICE] ws connected (%s) heap=%u\n", (const char *)payload, (unsigned)ESP.getFreeHeap());
      break;
    case WStype_DISCONNECTED:
      Serial.printf("[VOICE] ws disconnected (state %d) heap=%u min=%u tx=%lu rx=%lu rssi=%d\n", (int)s_state,
                    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(), (unsigned long)s_st.txChunks, (unsigned long)s_st.bytesIn,
                    WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
      if (s_state == VS_CONNECTING) { s_state = VS_ERROR; postEvent(Event::FAILED, "connect failed"); }
      else if (s_state == VS_READY) { s_state = VS_ERROR; postEvent(Event::CLOSED); }
      break;
    case WStype_TEXT: {
      s_st.bytesIn += len;
      const uint32_t h0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
      handleMessage((const char *)payload, len);
      if (len > 8192) Serial.printf("[VOICE] big frame %u B: int %u -> %u largest=%u\n", (unsigned)len, (unsigned)h0,
                                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
      break;
    }
    case WStype_FRAGMENT_TEXT_START:
      s_fragLen = 0;
      s_fragOverflow = false;
      // fallthrough
    case WStype_FRAGMENT:
      if (!s_frag || s_fragLen + len >= FRAG_CAP) { s_fragOverflow = true; break; }
      memcpy(s_frag + s_fragLen, payload, len);
      s_fragLen += len;
      break;
    case WStype_FRAGMENT_FIN:
      if (s_frag && !s_fragOverflow && s_fragLen + len < FRAG_CAP) {
        memcpy(s_frag + s_fragLen, payload, len); s_fragLen += len; s_frag[s_fragLen] = 0;
        s_st.bytesIn += s_fragLen;
        handleMessage(s_frag, s_fragLen);
      } else if (s_fragOverflow) {
        Serial.printf("[VOICE] dropped fragmented frame > %u B\n", (unsigned)FRAG_CAP);
      }
      s_fragLen = 0;
      s_fragOverflow = false;
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
uint32_t s_sendMaxUs = 0, s_sendSlow = 0, s_hookSends = 0, s_sendBusy = 0;   // upload timing diagnostics

void sendMicChunk() {
  // 40 ms of 16 kHz PCM per append (small frames keep the TLS/WS send buffers
  // small: internal RAM is the scarce resource during a session)
  const int N = 640;
  if (ESP.getFreeHeap() < 24000) return;   // back-pressure: skip this round, the ring keeps the audio
  // The socket is reported writable only a fraction of the time (the server
  // streams 26 KB audio frames over the same connection, and the public uplink
  // stalls for seconds at a time). Never block here: force-writing while the
  // peer is not draining froze this task for seconds, which also stopped it
  // from processing incoming events (barge-in). The loop instead polls faster
  // while a backlog exists, so a short writable window is not missed.
  if (!s_ws.writable()) { s_sendBusy++; return; }
  static int16_t *pcm = (int16_t *)heap_caps_malloc(N * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!pcm) return;
  xSemaphoreTake(s_ringLock, portMAX_DELAY);
  if (s_micCount < N) { xSemaphoreGive(s_ringLock); return; }
  int head = s_micHead;
  int first = min(N, MIC_CAP - head);
  memcpy(pcm, &s_mic[head], first * sizeof(int16_t));
  if (N > first) memcpy(pcm + first, &s_mic[0], (N - first) * sizeof(int16_t));
  s_micHead = (head + N) % MIC_CAP;
  s_micCount -= N;
  xSemaphoreGive(s_ringLock);
  const size_t B64CAP = N * 2 / 3 * 4 + 8, MSGCAP = B64CAP + 64;
  static char *b64 = (char *)heap_caps_malloc(B64CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  static char *msg = (char *)heap_caps_malloc(MSGCAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!b64 || !msg) return;
  size_t olen = 0;
  if (mbedtls_base64_encode((uint8_t *)b64, B64CAP, &olen, (const uint8_t *)pcm, N * 2) != 0) return;
  int n = snprintf(msg, MSGCAP, "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%.*s\"}", (int)olen, b64);
  const uint32_t t0 = micros();
  const bool okSend = s_ws.sendTXT(msg, n);
  const uint32_t dt = micros() - t0;
  if (dt > s_sendMaxUs) s_sendMaxUs = dt;
  if (dt > 100000) s_sendSlow++;
  if (okSend) s_lastSendMs = millis();
  if (!okSend) {
    Serial.printf("[VOICE] sendTXT failed (write stalled >5 s) int=%u largest=%u -> closing\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    s_ws.disconnect();   // a half-written frame cannot be recovered
    return;
  }
  s_st.txChunks++;
}

// Runs inside the WebSocket read loop while it waits for the rest of a large
// incoming frame (a 400 ms audio delta is ~26 KB and can take 100-300 ms to
// arrive). Without this the mic upload stalls for that long on every frame,
// the server hears the guest late and in pieces, and barge-in never triggers.
void wsIdleHook() {
  static bool inHook = false;
  if (inHook || !s_sessionReady || !s_ws.isConnected()) return;
  inHook = true;
  // Control messages (response.cancel, tool results) must not wait for a 26 KB
  // audio frame to finish arriving: on a weak link that read can take seconds,
  // and a late cancel leaves the server talking and the turn state confused.
  String *js = nullptr;
  while (s_txq && xQueueReceive(s_txq, &js, 0) == pdTRUE) {
    if (s_ws.isConnected()) s_ws.sendTXT(js->c_str(), js->length());
    if (js->length() < 80) Serial.printf("[VOICE] >> %s\n", js->c_str());
    delete js;
  }
  const uint32_t before = s_st.txChunks;
  sendMicChunk();
  if (s_st.txChunks != before) s_hookSends++;
  inHook = false;
}

void voiceTask(void *) {
  // While the session runs, let malloc() of 2 KB+ come from PSRAM (default is
  // 4 KB+ internal first). mbedTLS record buffers and the lwIP/WS copies are the
  // big internal-RAM consumers; audio DMA and WiFi keep their own internal pools.
  heap_caps_malloc_extmem_enable(2048);
  String url = "/api-ws/v1/realtime?model=" + s_cfg.model;
  String hdr = "Authorization: Bearer " + s_cfg.apiKey;
  s_ws.setExtraHeaders(hdr.c_str());
  s_ws.setReconnectInterval(3000);
  s_ws.enableHeartbeat(30000, 20000, 3);   // lenient: long frame reads / blocked writes must not count as a dead link
  s_ws.onEvent(onWsEvent);
  WebSockets::idleHook = wsIdleHook;
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
      if (js->length() < 80) Serial.printf("[VOICE] >> %s\n", js->c_str());
      delete js;
    }
    if (s_sessionReady && s_ws.isConnected()) {
      sendMicChunk();
      sendMicChunk();
      static uint32_t lastTxLog = 0, minInt = 0xFFFFFFFF;
      const uint32_t hi = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
      if (hi < minInt) minInt = hi;
      if (millis() - lastTxLog > 2000) {
        lastTxLog = millis();
        Serial.printf("[VOICE] tx=%lu hook=%lu busy=%lu sendmax=%lums slow=%lu stall=%lus | int now=%u min2s=%u largest=%u | total min=%u\n", (unsigned long)s_st.txChunks,
                      (unsigned long)s_hookSends, (unsigned long)s_sendBusy, (unsigned long)(s_sendMaxUs / 1000), (unsigned long)s_sendSlow,
                      (unsigned long)(s_lastSendMs ? (millis() - s_lastSendMs) / 1000 : 0),
                      (unsigned)hi, (unsigned)minInt, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned)ESP.getMinFreeHeap());
        minInt = 0xFFFFFFFF; s_sendMaxUs = 0;
      }
    } else if (s_state == VS_CONNECTING && millis() - started > VOICE_CONNECT_TIMEOUT_MS) {
      Serial.println("[VOICE] connect timeout");
      s_state = VS_ERROR;
      postEvent(Event::FAILED, "connect timeout");
      break;
    } else if (s_state == VS_ERROR) {
      break;
    }
    // Poll fast while audio is queued: the send direction is only briefly
    // writable, and a 10 ms tick would miss most of those windows.
    vTaskDelay(pdMS_TO_TICKS(s_micCount > 640 ? 2 : 10));
  }
  WebSockets::idleHook = nullptr;
  s_ws.disconnect();
  heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
  // drain tx queue
  String *js = nullptr;
  while (s_txq && xQueueReceive(s_txq, &js, 0) == pdTRUE) delete js;
  s_sessionReady = false;
  s_responseActive = false;
  if (s_state != VS_ERROR) s_state = VS_OFF;
  Serial.printf("[VOICE] task exit state=%d heap=%u\n", (int)s_state, (unsigned)ESP.getFreeHeap());
  s_taskDone = true;
  for (;;) vTaskDelay(pdMS_TO_TICKS(1000));   // reaped with vTaskDeleteWithCaps() (PSRAM stack)
}

}  // namespace

// ===========================================================================
bool VoiceSession::begin(const Config &cfg) {
  reapTask();
  if (s_task) end();
  if (!s_ringLock) s_ringLock = xSemaphoreCreateMutex();
  if (!s_evq) s_evq = xQueueCreate(16, sizeof(Event *));
  if (!s_txq) s_txq = xQueueCreate(8, sizeof(String *));
  if (!s_mic) s_mic = (int16_t *)heap_caps_malloc(MIC_CAP * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_frag) s_frag = (char *)heap_caps_malloc(FRAG_CAP + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  s_cfg = cfg;
  if (s_cfg.outRate < 8000 || s_cfg.outRate > 48000) s_cfg.outRate = 24000;
  int cap = (int)s_cfg.outRate * 4;      // 4 s of playback buffer is plenty
  if (cap != s_playCap) { free(s_play); s_play = (int16_t *)heap_caps_malloc(cap * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); s_playCap = s_play ? cap : 0; }
  s_prerollSamples = (int)s_cfg.outRate * VOICE_PREROLL_MS / 1000;
  if (!s_mic || !s_play || !s_frag) { Serial.println("[VOICE] buffer alloc failed"); return false; }
  if (!cfg.apiKey.length()) { Serial.println("[VOICE] no API key configured"); s_state = VS_ERROR; postEvent(Event::FAILED, "no api key"); return false; }
  ringsReset();
  memset(&s_st, 0, sizeof(s_st));
  s_fragLen = 0; s_fragOverflow = false;
  s_dropAudio = false;
  s_sessionReady = false;
  s_responseActive = false;
  s_retryCreate = false;
  s_stopReq = false;
  s_taskDone = false;
  s_lastUser = millis();
  s_lastSendMs = millis();
  s_beginMs = millis();
  s_state = VS_CONNECTING;
  // Core 0 next to WiFi; priority between the audio task (12) and loop().
  if (xTaskCreatePinnedToCoreWithCaps(voiceTask, "voice", 16384, nullptr, 6, &s_task, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    s_state = VS_ERROR; s_task = nullptr; return false;
  }
  return true;
}

void VoiceSession::end() {
  if (s_task) {
    s_stopReq = true;
    // The task normally exits in one 10 ms tick. Cap the wait so a stuck socket
    // cannot freeze the UI: if it is still running, poll() reaps it later.
    for (int i = 0; i < 80 && !s_taskDone; i++) vTaskDelay(pdMS_TO_TICKS(10));
    reapTask();
  }
  s_state = VS_OFF;
  s_sessionReady = false;
  s_responseActive = false;
  s_dropAudio = false;
  if (s_ringLock) ringsReset();
  Event *e = nullptr;
  while (s_evq && xQueueReceive(s_evq, &e, 0) == pdTRUE) delete e;
}

bool VoiceSession::poll(Event &ev) {
  reapTask();
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

void VoiceSession::bargeIn() {
  s_dropAudio = true;                 // drop any reply frames still in flight
  flushPlayback();                    // and everything already buffered
  if (s_responseActive) queueSend("{\"type\":\"response.cancel\"}");
}

void VoiceSession::flushPlayback() {
  if (!s_ringLock) return;
  xSemaphoreTake(s_ringLock, portMAX_DELAY);
  s_playHead = s_playCount = 0;
  s_playing = false;
  xSemaphoreGive(s_ringLock);
}

void VoiceSession::pushMic(const int16_t *pcm, int n) {
  if (!s_mic || !s_sessionReady || n <= 0) return;
  xSemaphoreTake(s_ringLock, portMAX_DELAY);
  int head = s_micHead, count = s_micCount;
  // Stale audio only adds delay to the server's view of the guest, which costs
  // barge-in latency; keep at most MIC_MAX_BACKLOG and drop the oldest.
  if (count + n > MIC_MAX_BACKLOG) {
    int drop = count + n - MIC_MAX_BACKLOG;
    head = (head + drop) % MIC_CAP; count -= drop; s_st.micDrops += drop;
  }
  int tail = (head + count) % MIC_CAP;
  int first = min(n, MIC_CAP - tail);
  memcpy(&s_mic[tail], pcm, first * sizeof(int16_t));
  if (n > first) memcpy(&s_mic[0], pcm + first, (n - first) * sizeof(int16_t));
  s_micHead = head;
  s_micCount = count + n;
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
    // Partial underrun (network jitter): pad this frame with silence but keep
    // playing, so the next delta resumes immediately instead of waiting for a
    // fresh 240 ms preroll. An empty ring stops playback as before.
    int got = s_playCount;
    for (int i = 0; i < got; i++) { out[i] = s_play[s_playHead]; s_playHead = (s_playHead + 1) % s_playCap; }
    for (int i = got; i < n; i++) out[i] = 0;
    s_playCount = 0;
    if (got == 0) s_playing = false;
    xSemaphoreGive(s_ringLock);
    if (got == 0) return 0;
    s_st.playUnderruns++;
    return n;
  }
  int head = s_playHead;
  int first = min(n, s_playCap - head);
  memcpy(out, &s_play[head], first * sizeof(int16_t));
  if (n > first) memcpy(out + first, &s_play[0], (n - first) * sizeof(int16_t));
  s_playHead = (head + n) % s_playCap;
  s_playCount -= n;
  xSemaphoreGive(s_ringLock);
  return n;
}

bool VoiceSession::playbackActive() { return s_playing || s_playCount >= s_prerollSamples; }
uint32_t VoiceSession::playbackRate() { return s_cfg.outRate; }
uint32_t VoiceSession::lastUserSpeechMs() { return s_lastUser; }

uint32_t VoiceSession::uplinkStallMs() {
  if (!s_lastSendMs) return 0;
  uint32_t now = millis();
  return now - s_lastSendMs;
}

VoiceSession::Stats VoiceSession::stats() {
  Stats s = s_st;
  s.playDepthMs = s_playCap ? (int)((int64_t)s_playCount * 1000 / s_cfg.outRate) : 0;
  return s;
}
