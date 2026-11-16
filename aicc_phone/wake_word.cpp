#include "wake_word.h"
#include "config.h"
#include <string.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern "C" {
#include "model_path.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
}

namespace {

portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

srmodel_list_t *s_models = nullptr;

// WakeNet
const esp_wn_iface_t *s_wn = nullptr;
model_iface_data_t *s_wnData = nullptr;
int s_wnChunk = 0;
int16_t *s_wnBuf = nullptr;
int s_wnHave = 0;
float s_wnDefaultThr = 0.f;
uint8_t s_sensitivity = DEF_WAKE_SENSITIVITY;

// MultiNet
const esp_mn_iface_t *s_mn = nullptr;
model_iface_data_t *s_mnData = nullptr;
int s_mnChunk = 0;
int16_t *s_mnBuf = nullptr;
int s_mnHave = 0;

volatile WakeWord::Stage s_stage = WakeWord::STAGE_OFF;
volatile bool s_listen = true;

WakeWord::Stats s_st;
uint32_t s_wnUs = 0, s_wnN = 0, s_mnUs = 0, s_mnN = 0;

// pending detection handed to the main loop
volatile bool s_pending = false;
int s_pendCmd = -1;
float s_pendProb = 0.f;
char s_pendPhrase[48] = {};

void setError(const char *msg) {
  portENTER_CRITICAL(&s_lock);
  strlcpy(s_st.error, msg, sizeof(s_st.error));
  s_st.ready = false;
  portEXIT_CRITICAL(&s_lock);
  Serial.printf("[WAKE] unavailable: %s\n", msg);
}

void post(int cmd, float prob, const char *phrase) {
  portENTER_CRITICAL(&s_lock);
  s_pending = true;
  s_pendCmd = cmd;
  s_pendProb = prob;
  strlcpy(s_pendPhrase, phrase ? phrase : "", sizeof(s_pendPhrase));
  if (cmd == WAKE_CMD_WAKE) s_st.wakes++; else s_st.commands++;
  portEXIT_CRITICAL(&s_lock);
}

void applySensitivity() {
  if (!s_wnData) return;
  // Model default comes from _MODEL_INFO_ (DET_MODE_90 value). Relaxed lowers it a
  // little, strict uses the model's 95% value which the library applies itself when
  // created in DET_MODE_95; we emulate it with +0.01 on the default.
  float thr = s_wnDefaultThr;
  if (s_sensitivity == 1) thr -= 0.05f;
  else if (s_sensitivity == 2) thr += 0.01f;
  if (thr < 0.3f) thr = 0.3f;
  if (thr > 0.95f) thr = 0.95f;
  s_wn->set_det_threshold(s_wnData, thr, 1);
  Serial.printf("[WAKE] sensitivity %u -> threshold %.3f (model default %.3f)\n", s_sensitivity, (double)thr, (double)s_wnDefaultThr);
}

void feedWake(const int16_t *pcm, int samples) {
  while (samples > 0) {
    int n = samples < (s_wnChunk - s_wnHave) ? samples : (s_wnChunk - s_wnHave);
    memcpy(s_wnBuf + s_wnHave, pcm, n * sizeof(int16_t));
    s_wnHave += n; pcm += n; samples -= n;
    if (s_wnHave < s_wnChunk) continue;
    s_wnHave = 0;

    const uint32_t t0 = micros();
    const wakenet_state_t st = s_wn->detect(s_wnData, s_wnBuf);
    const uint32_t dt = micros() - t0;
    s_wnUs += dt; s_wnN++;
    portENTER_CRITICAL(&s_lock);
    s_st.chunks++;
    if (dt > s_st.wnMaxUs) s_st.wnMaxUs = dt;
    portEXIT_CRITICAL(&s_lock);

    if (st == WAKENET_DETECTED) {
      Serial.printf("[WAKE] WAKE WORD \"%s\" detect=%luus\n", s_st.wakeWord, (unsigned long)dt);
      post(WAKE_CMD_WAKE, 1.f, s_st.wakeWord);
      s_wn->clean(s_wnData);
      // Stay in STAGE_WAKE until the application opens the window: samples
      // arriving in between (the chime) are discarded by the audio task anyway.
      return;
    }
  }
}

void feedCommand(const int16_t *pcm, int samples) {
  while (samples > 0) {
    int n = samples < (s_mnChunk - s_mnHave) ? samples : (s_mnChunk - s_mnHave);
    memcpy(s_mnBuf + s_mnHave, pcm, n * sizeof(int16_t));
    s_mnHave += n; pcm += n; samples -= n;
    if (s_mnHave < s_mnChunk) continue;
    s_mnHave = 0;

    const uint32_t t0 = micros();
    const esp_mn_state_t state = s_mn->detect(s_mnData, s_mnBuf);
    const uint32_t dt = micros() - t0;
    s_mnUs += dt; s_mnN++;
    portENTER_CRITICAL(&s_lock);
    s_st.chunks++;
    if (dt > s_st.mnMaxUs) s_st.mnMaxUs = dt;
    portEXIT_CRITICAL(&s_lock);

    if (state == ESP_MN_STATE_DETECTED) {
      esp_mn_results_t *r = s_mn->get_results(s_mnData);
      const int cmd = (r && r->num > 0) ? r->command_id[0] : -1;
      const float prob = (r && r->num > 0) ? r->prob[0] : 0.f;
      const char *phrase = (r && r->num > 0) ? r->string : "";
      Serial.printf("[WAKE] COMMAND cmd=%d \"%s\" prob=%.2f detect=%luus\n", cmd, phrase, (double)prob, (unsigned long)dt);
      if (cmd > 0 && prob >= WAKE_MIN_PROB) post(cmd, prob, phrase);
      s_mn->clean(s_mnData);
    } else if (state == ESP_MN_STATE_TIMEOUT) {
      // MultiNet's own silence timeout: just restart it; the application's
      // window timer decides when the stage ends.
      s_mn->clean(s_mnData);
    }
  }
}

}  // namespace

bool WakeWord::begin() {
  if (!WAKE_WORD_ENABLED) { setError("disabled by config"); return false; }

  s_models = esp_srmodel_init("model");
  if (!s_models || s_models->num == 0) {
    setError("no model pack in the 'model' partition");
    Serial.println("[WAKE] flash tools/srmodels/srmodels_cn.bin with tools/flash.sh --models");
    return false;
  }
  Serial.printf("[WAKE] model partition: %d pack(s)\n", s_models->num);
  for (int i = 0; i < s_models->num; i++) Serial.printf("[WAKE]   [%d] %s\n", i, s_models->model_name[i]);

  // ---- WakeNet ----
  char *wnName = esp_srmodel_filter(s_models, ESP_WN_PREFIX, NULL);
  if (!wnName) { setError("no WakeNet model in the pack (rebuild with tools/pack_models.sh)"); return false; }
  s_wn = esp_wn_handle_from_name(wnName);
  s_wnData = s_wn ? s_wn->create(wnName, DET_MODE_90) : nullptr;
  if (!s_wnData) { setError("WakeNet create failed (out of PSRAM?)"); return false; }
  s_wnChunk = s_wn->get_samp_chunksize(s_wnData);
  if (s_wnChunk <= 0 || s_wnChunk > 2048) { setError("unexpected WakeNet chunk size"); return false; }
  s_wnBuf = (int16_t *)heap_caps_malloc(s_wnChunk * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!s_wnBuf) { setError("no RAM for the WakeNet buffer"); return false; }
  s_wnDefaultThr = s_wn->get_det_threshold(s_wnData, 1);
  const char *ww = esp_wn_wakeword_from_name(wnName);

  // ---- MultiNet (loaded now, run only inside the command window) ----
  char *mnName = esp_srmodel_filter(s_models, ESP_MN_PREFIX, ESP_MN_CHINESE);
  if (!mnName) { setError("no Chinese MultiNet in the pack"); return false; }
  s_mn = esp_mn_handle_from_name(mnName);
  s_mnData = s_mn ? s_mn->create(mnName, WAKE_MN_TIMEOUT_MS) : nullptr;
  if (!s_mnData) { setError("MultiNet create failed (out of PSRAM?)"); return false; }
  s_mnChunk = s_mn->get_samp_chunksize(s_mnData);
  if (s_mnChunk <= 0 || s_mnChunk > 2048) { setError("unexpected MultiNet chunk size"); return false; }
  s_mnBuf = (int16_t *)heap_caps_malloc(s_mnChunk * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!s_mnBuf) { setError("no RAM for the MultiNet buffer"); return false; }
  s_mn->set_det_threshold(s_mnData, WAKE_DET_THRESHOLD);

  portENTER_CRITICAL(&s_lock);
  strlcpy(s_st.wakeModel, wnName, sizeof(s_st.wakeModel));
  strlcpy(s_st.wakeWord, ww ? ww : "?", sizeof(s_st.wakeWord));
  strlcpy(s_st.cmdModel, mnName, sizeof(s_st.cmdModel));
  s_st.error[0] = 0;
  s_st.ready = true;
  s_st.stage = STAGE_WAKE;
  portEXIT_CRITICAL(&s_lock);
  s_stage = STAGE_WAKE;
  applySensitivity();

  Serial.printf("[WAKE] ready wake=%s (\"%s\", chunk %d = %d ms) cmd=%s (chunk %d) heap=%lu psram=%lu\n",
                wnName, s_st.wakeWord, s_wnChunk, s_wnChunk * 1000 / 16000, mnName, s_mnChunk,
                (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getFreePsram());
  s_mn->print_active_speech_commands(s_mnData);
  Serial.println("[WAKE] listening for the wake word");
  return true;
}

void WakeWord::setListening(bool on) {
  if (s_listen == on) return;
  s_listen = on;
  if (s_wnData) s_wn->clean(s_wnData);
  if (s_mnData) s_mn->clean(s_mnData);
  s_wnHave = s_mnHave = 0;
  if (on && s_stage == STAGE_COMMAND) s_stage = STAGE_WAKE;   // a call interrupted the window
  portENTER_CRITICAL(&s_lock);
  s_st.listening = on;
  s_st.stage = s_stage;
  portEXIT_CRITICAL(&s_lock);
  Serial.printf("[WAKE] listening %s\n", on ? "on" : "off");
}

bool WakeWord::listening() { return s_listen; }

void WakeWord::startCommandWindow() {
  if (!s_mnData) return;
  s_mn->clean(s_mnData);
  s_mnHave = 0;
  s_stage = STAGE_COMMAND;
  portENTER_CRITICAL(&s_lock);
  s_st.stage = STAGE_COMMAND;
  portEXIT_CRITICAL(&s_lock);
  Serial.println("[WAKE] command window open");
}

void WakeWord::endCommandWindow() {
  if (s_stage != STAGE_COMMAND) return;
  if (s_mnData) s_mn->clean(s_mnData);
  if (s_wnData) s_wn->clean(s_wnData);
  s_wnHave = s_mnHave = 0;
  s_stage = STAGE_WAKE;
  portENTER_CRITICAL(&s_lock);
  s_st.stage = STAGE_WAKE;
  portEXIT_CRITICAL(&s_lock);
  Serial.println("[WAKE] command window closed, back to wake word");
}

WakeWord::Stage WakeWord::stage() { return s_stage; }

void WakeWord::feed(const int16_t *pcm, int samples) {
  if (!s_listen || s_stage == STAGE_OFF) return;
  if (s_stage == STAGE_COMMAND) { if (s_mnData && s_mnBuf) feedCommand(pcm, samples); }
  else { if (s_wnData && s_wnBuf) feedWake(pcm, samples); }
}

bool WakeWord::consume(int &commandId, float &prob, char *phrase, size_t cap) {
  bool got = false;
  portENTER_CRITICAL(&s_lock);
  if (s_pending) {
    got = true;
    commandId = s_pendCmd;
    prob = s_pendProb;
    if (phrase && cap) strlcpy(phrase, s_pendPhrase, cap);
    s_pending = false;
  }
  portEXIT_CRITICAL(&s_lock);
  return got;
}

void WakeWord::setSensitivity(uint8_t level) {
  s_sensitivity = level > 2 ? 0 : level;
  applySensitivity();
}

WakeWord::Stats WakeWord::stats() {
  portENTER_CRITICAL(&s_lock);
  Stats s = s_st;
  s.wnAvgUs = s_wnN ? s_wnUs / s_wnN : 0;
  s.mnAvgUs = s_mnN ? s_mnUs / s_mnN : 0;
  s_wnUs = s_wnN = s_mnUs = s_mnN = 0;
  s_st.wnMaxUs = s_st.mnMaxUs = 0;
  portEXIT_CRITICAL(&s_lock);
  return s;
}
