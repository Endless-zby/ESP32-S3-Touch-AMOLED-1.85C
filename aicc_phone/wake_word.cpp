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
}

namespace {
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
srmodel_list_t *s_models = nullptr;
const esp_wn_iface_t *s_wn = nullptr;
model_iface_data_t *s_wnData = nullptr;
int s_chunk = 0;
int16_t *s_buf = nullptr;
int s_have = 0;
float s_defaultThr = 0.f;
uint8_t s_sensitivity = DEF_WAKE_SENSITIVITY;
volatile bool s_listen = true;
volatile bool s_pending = false;
WakeWord::Stats s_st;
uint32_t s_us = 0, s_n = 0;

void setError(const char *msg) {
  portENTER_CRITICAL(&s_lock);
  strlcpy(s_st.error, msg, sizeof(s_st.error));
  s_st.ready = false;
  portEXIT_CRITICAL(&s_lock);
  Serial.printf("[WAKE] unavailable: %s\n", msg);
}

void applySensitivity() {
  if (!s_wnData) return;
  float thr = s_defaultThr;
  if (s_sensitivity == 1) thr -= 0.05f;
  else if (s_sensitivity == 2) thr += 0.01f;
  if (thr < 0.3f) thr = 0.3f;
  if (thr > 0.95f) thr = 0.95f;
  s_wn->set_det_threshold(s_wnData, thr, 1);
  Serial.printf("[WAKE] sensitivity %u -> threshold %.3f (model default %.3f)\n", s_sensitivity, (double)thr, (double)s_defaultThr);
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
  for (int i = 0; i < s_models->num; i++) Serial.printf("[WAKE]   model[%d] %s\n", i, s_models->model_name[i]);
  char *wnName = esp_srmodel_filter(s_models, ESP_WN_PREFIX, NULL);
  if (!wnName) { setError("no WakeNet model in the pack (rebuild with tools/pack_models.sh)"); return false; }
  s_wn = esp_wn_handle_from_name(wnName);
  s_wnData = s_wn ? s_wn->create(wnName, DET_MODE_90) : nullptr;
  if (!s_wnData) { setError("WakeNet create failed (out of PSRAM?)"); return false; }
  s_chunk = s_wn->get_samp_chunksize(s_wnData);
  if (s_chunk <= 0 || s_chunk > 2048) { setError("unexpected WakeNet chunk size"); return false; }
  s_buf = (int16_t *)heap_caps_malloc(s_chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!s_buf) { setError("no RAM for the WakeNet buffer"); return false; }
  s_defaultThr = s_wn->get_det_threshold(s_wnData, 1);
  const char *ww = esp_wn_wakeword_from_name(wnName);
  portENTER_CRITICAL(&s_lock);
  strlcpy(s_st.model, wnName, sizeof(s_st.model));
  strlcpy(s_st.word, (ww && strcmp(ww, wnName) != 0) ? ww : "nihaoxiaoan", sizeof(s_st.word));
  s_st.error[0] = 0;
  s_st.ready = true;
  portEXIT_CRITICAL(&s_lock);
  applySensitivity();
  Serial.printf("[WAKE] ready %s (\"%s\", chunk %d = %d ms) heap=%lu psram=%lu\n", wnName, s_st.word, s_chunk,
                s_chunk * 1000 / 16000, (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getFreePsram());
  return true;
}

void WakeWord::setListening(bool on) {
  if (s_listen == on) return;
  s_listen = on;
  s_have = 0;
  // NOTE: wakenet9 clean() crashes in esp-sr 2.4.6 (model_clean dereferences a
  // freed convq queue), so the detector is simply left to re-arm itself.
  portENTER_CRITICAL(&s_lock);
  s_st.listening = on;
  portEXIT_CRITICAL(&s_lock);
  Serial.printf("[WAKE] listening %s\n", on ? "on" : "off");
}

bool WakeWord::listening() { return s_listen; }

void WakeWord::feed(const int16_t *pcm, int samples) {
  if (!s_wnData || !s_listen || !s_buf) return;
  while (samples > 0) {
    int n = samples < (s_chunk - s_have) ? samples : (s_chunk - s_have);
    memcpy(s_buf + s_have, pcm, n * sizeof(int16_t));
    s_have += n; pcm += n; samples -= n;
    if (s_have < s_chunk) continue;
    s_have = 0;
    const uint32_t t0 = micros();
    const wakenet_state_t st = s_wn->detect(s_wnData, s_buf);
    const uint32_t dt = micros() - t0;
    s_us += dt; s_n++;
    portENTER_CRITICAL(&s_lock);
    s_st.chunks++;
    if (dt > s_st.maxUs) s_st.maxUs = dt;
    portEXIT_CRITICAL(&s_lock);
    if (st == WAKENET_DETECTED) {
      Serial.printf("[WAKE] WAKE WORD \"%s\" detect=%luus\n", s_st.word, (unsigned long)dt);
      portENTER_CRITICAL(&s_lock);
      s_pending = true;
      s_st.wakes++;
      portEXIT_CRITICAL(&s_lock);
      return;
    }
  }
}

bool WakeWord::consume() {
  bool got = false;
  portENTER_CRITICAL(&s_lock);
  if (s_pending) { got = true; s_pending = false; }
  portEXIT_CRITICAL(&s_lock);
  return got;
}

void WakeWord::setSensitivity(uint8_t level) { s_sensitivity = level > 2 ? 0 : level; applySensitivity(); }

WakeWord::Stats WakeWord::stats() {
  portENTER_CRITICAL(&s_lock);
  Stats s = s_st;
  s.avgUs = s_n ? s_us / s_n : 0;
  s_us = s_n = 0;
  s_st.maxUs = 0;
  portEXIT_CRITICAL(&s_lock);
  return s;
}
