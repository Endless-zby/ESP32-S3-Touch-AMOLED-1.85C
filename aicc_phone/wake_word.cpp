#include "wake_word.h"
#include "config.h"
#include <string.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern "C" {
#include "model_path.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
}

namespace {

portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

srmodel_list_t *s_models = nullptr;
const esp_mn_iface_t *s_mn = nullptr;
model_iface_data_t *s_mnData = nullptr;

int s_chunk = 0;              // samples per detect() call
int16_t *s_buf = nullptr;     // chunk accumulator (internal RAM, DMA-safe not needed)
int s_have = 0;

WakeWord::Stats s_st;         // written from the audio task, guarded by s_lock
volatile bool s_listen = true;

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

}  // namespace

bool WakeWord::begin() {
  if (!WAKE_WORD_ENABLED) {
    setError("disabled by config");
    return false;
  }
  // Model pack flashed into the "model" partition (tools/srmodels/srmodels_cn.bin).
  s_models = esp_srmodel_init("model");
  if (!s_models || s_models->num == 0) {
    setError("no model pack in the 'model' partition");
    Serial.println("[WAKE] flash tools/srmodels/srmodels_cn.bin to the model partition (offset 0x410000)");
    return false;
  }
  Serial.printf("[WAKE] model partition: %d pack(s)\n", s_models->num);
  for (int i = 0; i < s_models->num; i++) Serial.printf("[WAKE]   [%d] %s\n", i, s_models->model_name[i]);

  char *mnName = esp_srmodel_filter(s_models, ESP_MN_PREFIX, ESP_MN_CHINESE);
  if (!mnName) {
    setError("no Chinese MultiNet in the model pack");
    return false;
  }

  s_mn = esp_mn_handle_from_name(mnName);
  s_mnData = s_mn ? s_mn->create(mnName, WAKE_MN_TIMEOUT_MS) : nullptr;
  if (!s_mnData) {
    setError("MultiNet model create failed (out of PSRAM?)");
    return false;
  }

  s_chunk = s_mn->get_samp_chunksize(s_mnData);
  if (s_chunk <= 0 || s_chunk > 2048) {
    setError("unexpected MultiNet chunk size");
    s_mn->destroy(s_mnData);
    s_mnData = nullptr;
    return false;
  }
  s_buf = (int16_t *)heap_caps_malloc(s_chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!s_buf) {
    setError("no internal RAM for the chunk buffer");
    s_mn->destroy(s_mnData);
    s_mnData = nullptr;
    return false;
  }
  s_have = 0;

  s_mn->set_det_threshold(s_mnData, WAKE_DET_THRESHOLD);
  Serial.printf("[WAKE] ready model=%s chunk=%d (%d ms) thr=%.2f heap=%lu psram=%lu\n",
                mnName, s_chunk, s_chunk * 1000 / 16000,
                (double)WAKE_DET_THRESHOLD,
                (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getFreePsram());
  s_mn->print_active_speech_commands(s_mnData);

  portENTER_CRITICAL(&s_lock);
  strlcpy(s_st.model, mnName, sizeof(s_st.model));
  strlcpy(s_st.error, "", sizeof(s_st.error));
  s_st.ready = true;
  portEXIT_CRITICAL(&s_lock);
  Serial.println("[WAKE] listening for the wake word");
  return true;
}

void WakeWord::setListening(bool on) {
  if (s_listen == on) return;
  s_listen = on;
  if (s_mnData) s_mn->clean(s_mnData);
  s_have = 0;
  portENTER_CRITICAL(&s_lock);
  s_st.listening = on;
  portEXIT_CRITICAL(&s_lock);
  Serial.printf("[WAKE] listening %s\n", on ? "on" : "off");
}

bool WakeWord::listening() { return s_listen; }

void WakeWord::feed(const int16_t *pcm, int samples) {
  if (!s_mnData || !s_listen || !s_buf) return;
  while (samples > 0) {
    int n = samples < (s_chunk - s_have) ? samples : (s_chunk - s_have);
    memcpy(s_buf + s_have, pcm, n * sizeof(int16_t));
    s_have += n;
    pcm += n;
    samples -= n;
    if (s_have < s_chunk) continue;
    s_have = 0;

    const uint32_t t0 = micros();
    const esp_mn_state_t state = s_mn->detect(s_mnData, s_buf);
    const uint32_t dt = micros() - t0;

    if (state == ESP_MN_STATE_DETECTED) {
      esp_mn_results_t *r = s_mn->get_results(s_mnData);
      const int cmd = (r && r->num > 0) ? r->command_id[0] : -1;
      const float prob = (r && r->num > 0) ? r->prob[0] : 0.f;
      const char *phrase = (r && r->num > 0) ? r->string : "";
      Serial.printf("[WAKE] DETECTED cmd=%d \"%s\" prob=%.2f detect=%luus\n",
                    cmd, phrase, (double)prob, (unsigned long)dt);
      if (cmd > 0 && prob >= WAKE_MIN_PROB) {
        portENTER_CRITICAL(&s_lock);
        s_pending = true;
        s_pendCmd = cmd;
        s_pendProb = prob;
        strlcpy(s_pendPhrase, phrase, sizeof(s_pendPhrase));
        s_st.detections++;
        portEXIT_CRITICAL(&s_lock);
      }
      s_mn->clean(s_mnData);
    } else if (state == ESP_MN_STATE_TIMEOUT) {
      s_mn->clean(s_mnData);
    }

    portENTER_CRITICAL(&s_lock);
    s_st.chunks++;
    if (dt > s_st.maxDetectUs) s_st.maxDetectUs = dt;
    portEXIT_CRITICAL(&s_lock);
  }
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

WakeWord::Stats WakeWord::stats() {
  portENTER_CRITICAL(&s_lock);
  Stats s = s_st;
  portEXIT_CRITICAL(&s_lock);
  return s;
}
