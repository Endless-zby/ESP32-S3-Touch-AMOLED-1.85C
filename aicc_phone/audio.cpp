#include "audio.h"
#include "config.h"
#include "board_io.h"
#include "ESP_I2S.h"
#include <math.h>
#include <string.h>
#include <esp_heap_caps.h>

#if BOARD_V2
#include "es8311.h"
#include "es7210.h"
#include "esp_aec.h"
static es8311_handle_t s_es8311 = NULL;
static es7210_dev_handle_t s_es7210 = NULL;
#endif

// V2 bus layout (one I2S port, 16 kHz, 16-bit stereo, 32 BCLK per frame):
//   TX  : L = R = speaker sample                 -> ES8311 DAC
//   RX  : L = MIC1 (voice), R = MIC3 (echo reference) <- ES7210 in TDM mode
// The ES7210 emits its channels in the order MIC1, MIC3, MIC2, MIC4 when TDM is
// enabled, and a standard stereo frame only has room for the first two, so the
// right channel carries MIC3. On this board MIC3 is wired to the ES8311 line
// output through an attenuator (see the "AEC" block of the V2 schematic), which
// gives the echo canceller an exact copy of what the speaker is playing.
static I2SClass s_i2s;       // V2: full duplex; V1: speaker
#if !BOARD_V2
static I2SClass s_i2s_mic;   // V1: MEMS mic on a second port
#endif

static volatile AudioMode s_mode = AUDIO_IDLE;
static volatile bool s_mute = false;
static volatile bool s_click = false;
static volatile uint8_t s_volume = DEF_VOLUME;
static volatile uint8_t s_micGain = DEF_MIC_GAIN;
static volatile uint8_t s_aecMode = DEF_AEC_MODE;
static volatile uint8_t s_micLevel = 0, s_spkLevel = 0;
static RtpSession *s_rtp = NULL;
static TaskHandle_t s_task = NULL;

// ---------------------------------------------------------------------------
// Codec bring-up
// ---------------------------------------------------------------------------
bool audio_init() {
#if BOARD_V2
  pinMode(PIN_PA_CTRL, OUTPUT);
  digitalWrite(PIN_PA_CTRL, LOW);

  s_es8311 = es8311_create(I2C_NUM_0, ES8311_ADDRESS_0);
  if (!s_es8311) { Serial.println("[AUDIO] es8311 create failed"); return false; }
  es8311_clock_config_t clk = {};
  clk.mclk_inverted = false;
  clk.sclk_inverted = false;
  clk.mclk_from_mclk_pin = true;
  clk.mclk_frequency = AUDIO_HW_RATE * 256;
  clk.sample_frequency = AUDIO_HW_RATE;
  if (es8311_init(s_es8311, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) {
    Serial.println("[AUDIO] es8311 init failed");
    return false;
  }
  es8311_voice_volume_set(s_es8311, s_volume, NULL);
  es8311_microphone_config(s_es8311, false);

  es7210_i2c_config_t i2ccfg = {};
  i2ccfg.i2c_port = I2C_NUM_0;
  i2ccfg.i2c_addr = ES7210_ADDRRES_00;
  if (es7210_new_codec(&i2ccfg, &s_es7210) != ESP_OK) {
    Serial.println("[AUDIO] es7210 create failed");
    return false;
  }
  es7210_codec_config_t cc = {};
  cc.i2s_format = ES7210_I2S_FMT_I2S;
  cc.mclk_ratio = 256;
  cc.sample_rate_hz = AUDIO_HW_RATE;
  cc.bit_width = ES7210_I2S_BITS_16B;
  cc.mic_bias = ES7210_MIC_BIAS_2V87;
  cc.mic_gain = ES7210_MIC_GAIN_30DB;
  cc.flags.tdm_enable = true;          // MIC1 + MIC3(reference) on one stereo frame
  if (es7210_config_codec(s_es7210, &cc) != ESP_OK) {
    Serial.println("[AUDIO] es7210 config failed");
    return false;
  }
  es7210_config_volume(s_es7210, 0);

  s_i2s.setPins(PIN_I2S_BCLK, PIN_I2S_LRCK, PIN_I2S_DOUT, PIN_I2S_DIN, PIN_I2S_MCLK);
  s_i2s.setTimeout(100);
  if (!s_i2s.begin(I2S_MODE_STD, AUDIO_HW_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("[AUDIO] i2s begin failed");
    return false;
  }
#else
  s_i2s.setPins(PIN_SPK_BCLK, PIN_SPK_LRCK, PIN_SPK_DOUT, -1, -1);
  if (!s_i2s.begin(I2S_MODE_STD, AUDIO_HW_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("[AUDIO] i2s spk begin failed");
    return false;
  }
  s_i2s_mic.setPins(PIN_MIC_BCLK, PIN_MIC_WS, -1, PIN_MIC_DIN, -1);
  s_i2s_mic.setTimeout(100);
  if (!s_i2s_mic.begin(I2S_MODE_STD, AUDIO_HW_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    Serial.println("[AUDIO] i2s mic begin failed");
    return false;
  }
#endif
  Serial.println("[AUDIO] init ok");
  return true;
}

static void pa_enable(bool on) {
#if BOARD_V2
  digitalWrite(PIN_PA_CTRL, on ? HIGH : LOW);
#endif
}

void audio_set_volume(uint8_t percent) {
  if (percent > 100) percent = 100;
  s_volume = percent;
#if BOARD_V2
  if (s_es8311) es8311_voice_volume_set(s_es8311, percent, NULL);
#endif
}

void audio_set_mic_gain(uint8_t percent) { s_micGain = percent; }
void audio_set_aec_mode(uint8_t mode) { s_aecMode = mode; }
uint8_t audio_aec_mode() { return s_aecMode; }
void audio_set_mute(bool m) { s_mute = m; }
bool audio_is_muted() { return s_mute; }
void audio_click() { s_click = true; }
uint8_t audio_mic_level() { return s_micLevel; }
uint8_t audio_spk_level() { return s_spkLevel; }
AudioMode audio_mode() { return s_mode; }

void audio_set_mode(AudioMode m) {
  s_mode = m;
}

// ---------------------------------------------------------------------------
// DSP helpers
// ---------------------------------------------------------------------------
static inline int16_t sat16(int32_t v) {
  if (v > 32767) return 32767;
  if (v < -32768) return -32768;
  return (int16_t)v;
}

// 16 kHz mono -> 8 kHz (3-tap smoothing then decimate)
static void downsample(const int16_t *in16, int16_t *out8, int16_t &hist) {
  for (int i = 0; i < FRAME_8K; i++) {
    int32_t a = (i == 0) ? hist : in16[2 * i - 1];
    int32_t b = in16[2 * i];
    int32_t c = in16[2 * i + 1];
    out8[i] = sat16((a + 2 * b + c) >> 2);
  }
  hist = in16[FRAME_16K - 1];
}

// 8 kHz -> 16 kHz linear interpolation
static void upsample(const int16_t *in8, int16_t *out16, int16_t &prev) {
  for (int i = 0; i < FRAME_8K; i++) {
    int32_t cur = in8[i];
    out16[2 * i] = sat16((prev + cur) >> 1);
    out16[2 * i + 1] = (int16_t)cur;
    prev = (int16_t)cur;
  }
}

static uint8_t level_of(const int16_t *pcm, int n) {
  int32_t peak = 0;
  for (int i = 0; i < n; i++) { int32_t v = pcm[i] < 0 ? -pcm[i] : pcm[i]; if (v > peak) peak = v; }
  uint32_t l = (uint32_t)peak * 100 / 32767;
  return l > 100 ? 100 : (uint8_t)l;
}

// Tone generator ------------------------------------------------------------
struct ToneGen {
  float phase1 = 0, phase2 = 0;
  uint32_t ms = 0;
  void reset() { phase1 = phase2 = 0; ms = 0; }
  // fill 20 ms @16 kHz mono; pattern given by on/off durations in ms
  void fill(int16_t *out, float f1, float f2, uint32_t onMs, uint32_t offMs, int16_t amp) {
    uint32_t period = onMs + offMs;
    bool on = (period == 0) || ((ms % period) < onMs);
    const float w1 = 2.0f * (float)M_PI * f1 / AUDIO_HW_RATE;
    const float w2 = 2.0f * (float)M_PI * f2 / AUDIO_HW_RATE;
    for (int i = 0; i < FRAME_16K; i++) {
      if (on) {
        float v = sinf(phase1);
        if (f2 > 0) v = 0.5f * (v + sinf(phase2));
        out[i] = (int16_t)(v * amp);
      } else out[i] = 0;
      phase1 += w1; if (phase1 > 2 * M_PI) phase1 -= 2 * (float)M_PI;
      phase2 += w2; if (phase2 > 2 * M_PI) phase2 -= 2 * (float)M_PI;
    }
    ms += FRAME_MS;
  }
};

// ---------------------------------------------------------------------------
// Acoustic echo cancellation (esp-sr), V2 only
// ---------------------------------------------------------------------------
#if BOARD_V2
// Simple FIFO of int16 samples
struct SampleFifo {
  int16_t *buf = nullptr;
  int cap = 0, head = 0, count = 0;
  bool init(int capacity) {
    cap = capacity;
    buf = (int16_t *)heap_caps_malloc(cap * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    head = count = 0;
    return buf != nullptr;
  }
  void clear() { head = count = 0; }
  void push(const int16_t *s, int n) {
    for (int i = 0; i < n; i++) {
      if (count == cap) { head = (head + 1) % cap; count--; }  // drop oldest
      buf[(head + count) % cap] = s[i];
      count++;
    }
  }
  int pop(int16_t *d, int n) {
    if (count < n) return 0;
    for (int i = 0; i < n; i++) { d[i] = buf[head]; head = (head + 1) % cap; }
    count -= n;
    return n;
  }
};

struct Aec {
  aec_handle_t *h = nullptr;
  uint8_t mode = 0;
  int chunk = 0;
  SampleFifo micIn, refIn, out;
  int16_t *bufMic = nullptr, *bufRef = nullptr, *bufOut = nullptr;
  uint32_t procUs = 0, procMaxUs = 0, procCount = 0;

  bool start(uint8_t m) {
    stop();
    if (m == 0) return false;
    aec_mode_t am = (m >= 2) ? AEC_MODE_VOIP_HIGH_PERF : AEC_MODE_VOIP_LOW_COST;
    aec_config_t cfg = {};
    cfg.mic_num = 1;
    cfg.ref_num = 1;
    cfg.out_num = 1;
    cfg.filter_length = 4;
    cfg.sample_rate = AUDIO_HW_RATE;
    cfg.caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;   // keep internal RAM for WiFi/lwIP
    cfg.mode = am;
    cfg.nlp_level = AEC_NLP_LEVEL_AGGR;
    h = aec_create_from_config(&cfg);
    if (!h) { Serial.println("[AEC] create failed"); return false; }
    chunk = aec_get_chunksize(h);
    if (chunk <= 0 || chunk > 2048) { Serial.printf("[AEC] bad chunk %d\n", chunk); stop(); return false; }
    int cap = chunk * 2 + FRAME_16K * 4;
    if (!micIn.init(cap) || !refIn.init(cap) || !out.init(cap)) { stop(); return false; }
    bufMic = (int16_t *)heap_caps_malloc(chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    bufRef = (int16_t *)heap_caps_malloc(chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    bufOut = (int16_t *)heap_caps_malloc(chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!bufMic || !bufRef || !bufOut) { stop(); return false; }
    mode = m;
    procUs = procMaxUs = procCount = 0;
    Serial.printf("[AEC] started mode=%d chunk=%d free=%u psram=%u\n", m, chunk, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
    return true;
  }
  void stop() {
    if (h) { aec_destroy(h); h = nullptr; }
    for (int16_t **p : {&bufMic, &bufRef, &bufOut, &micIn.buf, &refIn.buf, &out.buf}) {
      if (*p) { free(*p); *p = nullptr; }
    }
    micIn.cap = refIn.cap = out.cap = 0;
    mode = 0;
    chunk = 0;
  }
  bool active() const { return h != nullptr; }
  // Feed one 20 ms frame of mic + reference; returns true when a processed
  // frame is available in `cleaned`.
  bool process(const int16_t *mic, const int16_t *ref, int16_t *cleaned) {
    micIn.push(mic, FRAME_16K);
    refIn.push(ref, FRAME_16K);
    while (micIn.count >= chunk && refIn.count >= chunk) {
      micIn.pop(bufMic, chunk);
      refIn.pop(bufRef, chunk);
      uint32_t t0 = micros();
      aec_process(h, bufMic, bufRef, bufOut);
      uint32_t dt = micros() - t0;
      procUs += dt;
      if (dt > procMaxUs) procMaxUs = dt;
      procCount++;
      out.push(bufOut, chunk);
    }
    return out.pop(cleaned, FRAME_16K) == FRAME_16K;
  }
};
static Aec s_aec;
#endif

// ---------------------------------------------------------------------------
// Audio task: paced by the I2S DMA (20 ms per iteration)
// ---------------------------------------------------------------------------
static void write_stereo(const int16_t *mono16) {
  static int16_t st[FRAME_16K * 2];
  for (int i = 0; i < FRAME_16K; i++) { st[2 * i] = mono16[i]; st[2 * i + 1] = mono16[i]; }
  s_i2s.write((uint8_t *)st, sizeof(st));
}

// Reads one frame. mono16 = microphone, ref16 = echo reference (V2) or zeros.
static bool read_mic(int16_t *mono16, int16_t *ref16) {
#if BOARD_V2
  static int16_t st[FRAME_16K * 2];
  size_t got = s_i2s.readBytes((char *)st, sizeof(st));
  if (got < sizeof(st)) { memset(mono16, 0, FRAME_16K * 2); memset(ref16, 0, FRAME_16K * 2); return false; }
  for (int i = 0; i < FRAME_16K; i++) { mono16[i] = st[2 * i]; ref16[i] = st[2 * i + 1]; }
  return true;
#else
  static int32_t raw[FRAME_16K];
  memset(ref16, 0, FRAME_16K * 2);
  size_t got = s_i2s_mic.readBytes((char *)raw, sizeof(raw));
  if (got < sizeof(raw)) { memset(mono16, 0, FRAME_16K * 2); return false; }
  for (int i = 0; i < FRAME_16K; i++) mono16[i] = sat16(raw[i] >> 14);
  return true;
#endif
}

static void audio_task(void *) {
  static int16_t mic16[FRAME_16K], ref16[FRAME_16K], clean16[FRAME_16K], mic8[FRAME_8K];
  static int16_t spk8[FRAME_8K], spk16[FRAME_16K];
  static int16_t silence16[FRAME_16K] = {0};
  int16_t dsHist = 0, usPrev = 0;
  ToneGen tone;
  AudioMode last = AUDIO_IDLE;
  uint32_t echoHold = 0;       // ms of mic attenuation remaining after speaker activity (fallback when AEC is off)
  int clickFrames = 0;
  uint32_t lastStat = 0;
  uint8_t refLevel = 0;

  for (;;) {
    AudioMode m = s_mode;
    if (m != last) {
      tone.reset();
      dsHist = usPrev = 0;
      pa_enable(m != AUDIO_IDLE);
      if (m != AUDIO_IDLE) {
        // prefill TX so the DMA never starts empty
        write_stereo(silence16);
        write_stereo(silence16);
      }
#if BOARD_V2
      if (m == AUDIO_CALL) {
        if (s_aecMode != 0) s_aec.start(s_aecMode);
      } else if (s_aec.active()) {
        s_aec.stop();
      }
#endif
      last = m;
    }

    if (s_click) { s_click = false; clickFrames = 2; if (m == AUDIO_IDLE) pa_enable(true); }

    if (m == AUDIO_IDLE) {
      if (clickFrames > 0) {
        tone.fill(spk16, 1200, 0, 1000, 0, 6000);
        write_stereo(spk16);
        if (--clickFrames == 0) { vTaskDelay(pdMS_TO_TICKS(40)); pa_enable(false); tone.reset(); }
        continue;
      }
      s_micLevel = s_spkLevel = 0;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    // ---- capture (paces the loop) ----
    if (!read_mic(mic16, ref16)) vTaskDelay(pdMS_TO_TICKS(FRAME_MS));  // keep pacing even without a mic

    if (m == AUDIO_CALL && s_rtp) {
      // ---- uplink ----
      const int16_t *src = mic16;
      bool aecUsed = false;
#if BOARD_V2
      if (s_aec.active()) {
        if (s_aec.process(mic16, ref16, clean16)) src = clean16;
        else memset(clean16, 0, sizeof(clean16)), src = clean16;  // priming: send silence for the first chunk
        aecUsed = true;
      }
      refLevel = level_of(ref16, FRAME_16K);
#endif
      downsample(src, mic8, dsHist);
      int32_t g = s_micGain;  // percent
      bool suppress = !aecUsed && echoHold > 0;
      for (int i = 0; i < FRAME_8K; i++) {
        int32_t v = (mic8[i] * g) / 100;
        if (suppress) v = v / 16;
        if (s_mute) v = 0;
        mic8[i] = sat16(v);
      }
      s_micLevel = level_of(mic8, FRAME_8K);
      s_rtp->sendFrame(mic8);

      // ---- downlink ----
      s_rtp->pump();
      if (s_rtp->popFrame(spk8)) {
        s_spkLevel = level_of(spk8, FRAME_8K);
        if (s_spkLevel > 6) echoHold = 160; else if (echoHold >= FRAME_MS) echoHold -= FRAME_MS; else echoHold = 0;
        upsample(spk8, spk16, usPrev);
      } else {
        s_spkLevel = 0;
        if (echoHold >= FRAME_MS) echoHold -= FRAME_MS; else echoHold = 0;
        memset(spk16, 0, sizeof(spk16));
        usPrev = 0;
      }
      if (clickFrames > 0) { tone.fill(spk16, 1200, 0, 1000, 0, 5000); clickFrames--; }
      write_stereo(spk16);

#if BOARD_V2
      uint32_t now = millis();
      if (now - lastStat > 5000) {
        lastStat = now;
        if (s_aec.active() && s_aec.procCount) {
          Serial.printf("[AEC] avg %lu us / chunk(%d) max %lu us  mic=%u ref=%u spk=%u\n",
                        (unsigned long)(s_aec.procUs / s_aec.procCount), s_aec.chunk,
                        (unsigned long)s_aec.procMaxUs, s_micLevel, refLevel, s_spkLevel);
          s_aec.procUs = 0; s_aec.procCount = 0; s_aec.procMaxUs = 0;
        } else {
          Serial.printf("[AUDIO] mic=%u ref=%u spk=%u (aec off)\n", s_micLevel, refLevel, s_spkLevel);
        }
      }
#endif
      continue;
    }

    // ---- local tones ----
    switch (m) {
      case AUDIO_RINGBACK: tone.fill(spk16, 450, 0, 1000, 4000, 7000); break;          // CN ringback
      case AUDIO_RINGING:  tone.fill(spk16, 880, 1047, 1000, 2000, 9000); break;        // two-tone ring
      case AUDIO_BUSY:     tone.fill(spk16, 450, 0, 350, 350, 7000); break;
      default:             memset(spk16, 0, sizeof(spk16)); break;
    }
    if (clickFrames > 0) { tone.fill(spk16, 1200, 0, 1000, 0, 5000); clickFrames--; }
    s_spkLevel = level_of(spk16, FRAME_16K);
    s_micLevel = 0;
    write_stereo(spk16);
  }
}

void audio_start_task(RtpSession *rtp) {
  s_rtp = rtp;
  if (s_task) return;
  // Core 1: away from the WiFi/lwIP stack on core 0. LVGL (loop()) also runs on
  // core 1 but at a lower priority, so the 20 ms audio cadence is never starved.
  xTaskCreatePinnedToCore(audio_task, "audio", 12288, NULL, 12, &s_task, 1);
}
