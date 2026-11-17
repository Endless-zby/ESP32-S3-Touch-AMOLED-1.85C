#include "audio.h"
#include "config.h"
#include "board_io.h"
#include "wake_word.h"
#include "voice_session.h"
#include "ESP_I2S.h"
#include <math.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#if BOARD_V2
#include "es8311.h"
#include "es7210_board.h"
#include "esp_aec.h"
#include "esp_vad.h"
static es8311_handle_t s_es8311 = NULL;
#endif

// V2 bus layout (one I2S port, 16 kHz, 32-bit stereo => 64 BCLK per frame):
//   TX  : L = R = speaker sample in the top 16 bits   -> ES8311 DAC (32-bit mode)
//   RX  : ES7210 in 1xFS TDM mode packs four 16-bit ADC words into the 64-bit
//         frame: slot0 = L[31:16], slot1 = L[15:0], slot2 = R[31:16], slot3 = R[15:0]
// On this board MIC3 is wired to the ES8311 line output through an attenuator
// (see the "AEC" block of the V2 schematic): that slot is the echo reference.
// Which slot carries which ADC is selected at runtime (s_micSlot / s_refSlot)
// because the TDM ordering differs between documentation and silicon; the
// per-slot levels are printed on the serial console during a call.
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
static volatile uint8_t s_micSlot = 0;      // TDM slot carrying the voice microphone (0..3)
static volatile uint8_t s_refSlot = 4;      // 0..3 = TDM slot with the DAC loopback, 4 = software reference (our own playback)
static volatile uint16_t s_refDelayMs = DEF_REF_DELAY_MS;  // software reference delay
static volatile uint8_t s_aecFilter = DEF_AEC_FILTER;      // esp-sr filter_length
static volatile uint8_t s_refGain = DEF_REF_GAIN;          // ES7210 MIC3 (reference) PGA gain code
static volatile uint8_t s_micPga = DEF_MIC_PGA;            // ES7210 MIC1/MIC2 PGA gain code
static volatile uint8_t s_refDigGain = DEF_REF_DIG_GAIN;   // software reference gain
static volatile bool s_resSuppress = DEF_RES_SUPPRESS;
static bool s_codecReady = false;
static volatile uint8_t s_slotLevel[4] = {0, 0, 0, 0};
static volatile uint8_t s_micLevel = 0, s_spkLevel = 0;
// Double-talk measurement only (no action taken): tracks how far the raw
// microphone rises above the echo path learned while the far end plays
// (raw/ref ratio floor). Printed as g=/r= so the AEC tuning can be judged from
// the log; the actual cancellation stays entirely inside esp-sr's AEC.
static volatile uint16_t s_echoGain = 0;     // learned mic/ref ratio when only echo is present
static volatile uint8_t s_nearMaxRatio = 0;  // max raw/ref ratio seen in the window

// The xiaozhi-esp32 way: a model VAD (esp_vad, VAD_MODE_0 / 30 ms) runs on the
// *echo-cancelled* signal; when it reports speech while we are playing, that is
// the barge-in trigger (their AFE VAD drives the same "abort TTS" path).
struct Vad {
  vad_handle_t h = nullptr;
  int16_t buf[480]; int have = 0;                 // AEC gives 20 ms frames, VAD wants 30 ms
  volatile vad_state_t state = VAD_SILENCE;
  bool start() {
    if (h) return true;
    // VAD_MODE_0 with 30 ms min speech / 100 ms min noise: xiaozhi's AFE VAD
    // parameters (vad_min_noise_ms = 100).
    h = vad_create_with_param(VAD_MODE_0, 16000, 30, 60, 100);
    have = 0; state = VAD_SILENCE;
    return h != nullptr;
  }
  void stop() {
    if (h) { vad_destroy(h); h = nullptr; }
    have = 0; state = VAD_SILENCE;
  }
  void push(const int16_t *pcm, int n) {
    if (!h) return;
    while (n > 0) {
      int take = n < (480 - have) ? n : (480 - have);
      memcpy(buf + have, pcm, take * sizeof(int16_t));
      have += take; pcm += take; n -= take;
      if (have == 480) { state = vad_process(h, buf, 16000, 30); have = 0; }
    }
  }
};
static volatile bool s_bargeFlag = false;      // consumed by the assistant
static Vad s_vad;
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
  if (es8311_init(s_es8311, &clk, ES8311_RESOLUTION_32, ES8311_RESOLUTION_32) != ESP_OK) {
    Serial.println("[AUDIO] es8311 init failed");
    return false;
  }
  es8311_voice_volume_set(s_es8311, s_volume, NULL);
  es8311_microphone_config(s_es8311, false);

  // ES7210: four ADCs in TDM, programmed like esp_codec_dev (xiaozhi-esp32).
  if (!es7210_board_init(AUDIO_HW_RATE, s_micPga, s_refGain)) {
    Serial.println("[AUDIO] es7210 init failed");
    return false;
  }

  s_i2s.setPins(PIN_I2S_BCLK, PIN_I2S_LRCK, PIN_I2S_DOUT, PIN_I2S_DIN, PIN_I2S_MCLK);
  s_i2s.setTimeout(100);
  if (!s_i2s.begin(I2S_MODE_STD, AUDIO_HW_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO)) {
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
  s_codecReady = true;
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
void audio_set_slots(uint8_t micSlot, uint8_t refSlot) { s_micSlot = micSlot & 3; s_refSlot = refSlot > 4 ? 4 : refSlot; }
void audio_set_ref_delay(uint16_t ms) { s_refDelayMs = ms > 400 ? 400 : ms; }
void audio_set_aec_filter(uint8_t n) { s_aecFilter = n < 1 ? 1 : (n > 16 ? 16 : n); }
void audio_set_ref_gain(uint8_t code) {
  s_refGain = code > 14 ? 14 : code;
#if BOARD_V2
  if (s_codecReady) es7210_board_set_gain(3, s_refGain);
#endif
}
void audio_set_mic_pga(uint8_t code) {
  s_micPga = code > 14 ? 14 : code;
#if BOARD_V2
  if (s_codecReady) { es7210_board_set_gain(1, s_micPga); es7210_board_set_gain(2, s_micPga); }
#endif
}
void audio_set_ref_dig_gain(uint8_t x) { s_refDigGain = x < 1 ? 1 : (x > 8 ? 8 : x); }
void audio_set_res_suppress(bool on) { s_resSuppress = on; }
void audio_set_mute(bool m) { s_mute = m; }
void audio_click() { s_click = true; }
uint8_t audio_mic_level() { return s_micLevel; }
uint8_t audio_spk_level() { return s_spkLevel; }
AudioMode audio_mode() { return s_mode; }

// Barge-in (read-and-clear): the local VAD heard speech over the echo-cancelled
// signal while the assistant was playing. Mirrors xiaozhi's AFE VAD -> abort path.
bool audio_take_barge() {
  if (!s_bargeFlag) return false;
  s_bargeFlag = false;
  return true;
}

bool audio_vad_state() { return s_vad.state == VAD_SPEECH; }

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

// 24 kHz -> 16 kHz polyphase (2 phases x 64 taps, Q15). Flat to 7.5 kHz with
// >55 dB image rejection above 9 kHz; the previous 3:2 point sampling folded
// 8-12 kHz straight into the speech band. Group delay 32 input samples.
static const int16_t kRs24[2][64] = {
  {
        -9,     28,    -21,    -11,     43,    -37,    -17,     77,
       -70,    -27,    135,   -124,    -40,    222,   -208,    -55,
       347,   -335,    -70,    529,   -529,    -84,    809,   -848,
       -96,   1306,  -1478,   -104,   2560,  -3506,   -109,  18107,
     18107,   -109,  -3506,   2560,   -104,  -1478,   1306,    -96,
      -848,    809,    -84,   -529,    529,    -70,   -335,    347,
       -55,   -208,    222,    -40,   -124,    135,    -27,    -70,
        77,    -17,    -37,     43,    -11,    -21,     28,     -9,
  },
  {
       -26,     18,     10,    -35,     30,     15,    -62,     55,
        24,   -110,    100,     36,   -185,    171,     50,   -292,
       277,     65,   -445,    435,     79,   -670,    683,     92,
     -1035,   1118,    102,  -1775,   2142,    108,  -4549,   8977,
     21952,   8936,  -4507,    106,   2102,  -1734,     99,   1082,
      -997,     88,    650,   -635,     75,    407,   -414,     60,
       254,   -266,     45,    153,   -164,     31,     88,    -95,
        20,     47,    -53,     13,     26,    -31,      9,     18,
  },
};

static void resample24to16(const int16_t *in, int16_t *out) {
  const int nIn = FRAME_16K * 3 / 2;                    // 480 samples = 20 ms
  static int16_t hist[64];                              // tail of the previous frame
  static int16_t buf[64 + FRAME_16K * 3 / 2];
  memcpy(buf, hist, sizeof(hist));
  memcpy(buf + 64, in, nIn * sizeof(int16_t));
  for (int n = 0; n < FRAME_16K; n++) {
    const int base = (3 * n) / 2 + 1;                   // 64 + floor(1.5n) - 63
    const int16_t *h = kRs24[n & 1];
    int32_t acc = 0;
    for (int k = 0; k < 64; k++) acc += (int32_t)h[k] * buf[base + k];
    out[n] = sat16(acc >> 15);
  }
  memcpy(hist, buf + nIn, sizeof(hist));
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

// Wake acknowledgement chime: a sine plus a soft 2nd harmonic and an
// exponential decay, which sounds rounder than a plain gated sine.
struct PromptSeg { uint16_t freq; uint16_t ms; };
static const PromptSeg kPromptWake[] = {{1046, 110}, {0, 25}, {1568, 190}};

static volatile uint8_t s_promptReq = AUDIO_PROMPT_NONE;
static volatile bool s_promptBusy = false;
static const PromptSeg *s_promptSegs = nullptr;
static const int kPromptSegCount = 3;
static int s_promptSegIdx = 0;
static uint32_t s_promptSegMs = 0;
static float s_promptPhase = 0;

void audio_prompt(uint8_t kind) { s_promptReq = kind; }
bool audio_prompt_busy() { return s_promptBusy; }

// Render one 20 ms frame of the active prompt; returns false when finished.
static bool prompt_render(int16_t *out) {
  if (!s_promptSegs) return false;
  if (s_promptSegIdx >= kPromptSegCount) { s_promptSegs = nullptr; return false; }
  const PromptSeg &seg = s_promptSegs[s_promptSegIdx];
  const uint32_t segMs = seg.ms;
  memset(out, 0, FRAME_16K * sizeof(int16_t));
  if (seg.freq) {
    const float w = 2.0f * (float)M_PI * seg.freq / AUDIO_HW_RATE;
    for (int i = 0; i < FRAME_16K; i++) {
      const uint32_t t = s_promptSegMs + (uint32_t)((uint32_t)i * 1000u / AUDIO_HW_RATE);
      // 5 ms attack plus an exponential decay: a bell, not a buzz.
      float env = 1.0f;
      if (t < 5) env = t / 5.0f;
      else { float u = 1.f - (float)t / (float)segMs; if (u < 0.f) u = 0.f; env = u * u; }
      const float v = sinf(s_promptPhase) + 0.30f * sinf(2.f * s_promptPhase);
      out[i] = (int16_t)(v * 6200.0f * env);
      s_promptPhase += w;
      if (s_promptPhase > 2 * (float)M_PI) s_promptPhase -= 2 * (float)M_PI;
    }
  }
  s_promptSegMs += FRAME_MS;
  if (s_promptSegMs >= segMs) { s_promptSegIdx++; s_promptSegMs = 0; s_promptPhase = 0; }
  return s_promptSegIdx < kPromptSegCount;
}

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
    // 1 = full-duplex low cost + very aggressive NLP (what xiaozhi-esp32 uses on this board)
    // 2 = VoIP high performance, 3 = full-duplex high performance
    // The same filter xiaozhi-esp32 runs on this board (AFE_TYPE_FD low cost +
    // VERYAGGR). Barge-in is not expected to come from the server VAD: the local
    // esp_vad below listens on the echo-cancelled signal and triggers the cancel.
    aec_mode_t am = AEC_MODE_FD_LOW_COST;
    aec_nlp_level_t nlp = AEC_NLP_LEVEL_VERYAGGR;
    if (m == 2) { am = AEC_MODE_VOIP_HIGH_PERF; nlp = AEC_NLP_LEVEL_AGGR; }
    else if (m == 3) { am = AEC_MODE_FD_HIGH_PERF; nlp = AEC_NLP_LEVEL_VERYAGGR; }
    aec_config_t cfg = {};
    cfg.mic_num = 1;
    cfg.ref_num = 1;
    cfg.out_num = 1;
    cfg.filter_length = s_aecFilter;
    cfg.sample_rate = AUDIO_HW_RATE;
    cfg.caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;   // keep internal RAM for WiFi/lwIP
    cfg.mode = am;
    cfg.nlp_level = nlp;
    h = aec_create_from_config(&cfg);
    if (!h) { Serial.println("[AEC] create failed"); return false; }
    chunk = aec_get_chunksize(h);
    if (chunk <= 0 || chunk > 2048) { Serial.printf("[AEC] bad chunk %d\n", chunk); stop(); return false; }
    int cap = chunk * 2 + FRAME_16K * 4;
    if (!micIn.init(cap) || !refIn.init(cap) || !out.init(cap)) { stop(); return false; }
    // esp_aec.h: "allocate memory by heap_caps_aligned_alloc()" - the kernels use
    // aligned (SIMD) loads, and a misaligned buffer is both slower and unsound.
    bufMic = (int16_t *)heap_caps_aligned_alloc(16, chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    bufRef = (int16_t *)heap_caps_aligned_alloc(16, chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    bufOut = (int16_t *)heap_caps_aligned_alloc(16, chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!bufMic || !bufRef || !bufOut) { stop(); return false; }
    mode = m;
    procUs = procMaxUs = procCount = 0;
    Serial.printf("[AEC] started mode=%d filter=%d chunk=%d ref=%s delay=%ums free=%u psram=%u\n", m, (int)s_aecFilter, chunk,
                  s_refSlot == 4 ? "software" : "hw-slot", (unsigned)s_refDelayMs, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
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
#if BOARD_V2
static bool write_stereo(const int16_t *mono16) {
  static int32_t st[FRAME_16K * 2];
  for (int i = 0; i < FRAME_16K; i++) {
    int32_t v = ((int32_t)mono16[i]) << 16;
    st[2 * i] = v;
    st[2 * i + 1] = v;
  }
  return s_i2s.write((uint8_t *)st, sizeof(st)) == (int)sizeof(st);
}
#else
static bool write_stereo(const int16_t *mono16) {
  static int16_t st[FRAME_16K * 2];
  for (int i = 0; i < FRAME_16K; i++) { st[2 * i] = mono16[i]; st[2 * i + 1] = mono16[i]; }
  return s_i2s.write((uint8_t *)st, sizeof(st)) == (int)sizeof(st);
}
#endif

// Reads one frame. mono16 = microphone, ref16 = echo reference (V2) or zeros.
static bool read_mic(int16_t *mono16, int16_t *ref16) {
#if BOARD_V2
  static int32_t st[FRAME_16K * 2];
  size_t got = s_i2s.readBytes((char *)st, sizeof(st));
  if (got < sizeof(st)) { memset(mono16, 0, FRAME_16K * 2); memset(ref16, 0, FRAME_16K * 2); return false; }
  const uint8_t ms = s_micSlot, rs = s_refSlot < 4 ? s_refSlot : ms;
  int32_t peak[4] = {0, 0, 0, 0};
  for (int i = 0; i < FRAME_16K; i++) {
    uint32_t l = (uint32_t)st[2 * i], r = (uint32_t)st[2 * i + 1];
    int16_t slot[4] = {(int16_t)(l >> 16), (int16_t)(l & 0xFFFF), (int16_t)(r >> 16), (int16_t)(r & 0xFFFF)};
    mono16[i] = slot[ms];
    ref16[i] = slot[rs];   // overwritten by the software reference when s_refSlot == 4
    for (int k = 0; k < 4; k++) { int32_t a = slot[k] < 0 ? -slot[k] : slot[k]; if (a > peak[k]) peak[k] = a; }
  }
  for (int k = 0; k < 4; k++) { uint32_t lv = (uint32_t)peak[k] * 100 / 32767; s_slotLevel[k] = lv > 100 ? 100 : lv; }
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

#if BOARD_V2
// History of what we played (16 kHz), used as the software echo reference when
// s_refSlot == 4. 500 ms covers the maximum configurable delay.
#define PLAY_HIST_16K (AUDIO_HW_RATE / 2)
static int16_t *s_playHist16 = nullptr;   // PSRAM
static int s_playPos16 = 0;

static bool hist_alloc() {
  if (s_playHist16) return true;
  s_playHist16 = (int16_t *)heap_caps_calloc(PLAY_HIST_16K, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return s_playHist16 != nullptr;
}

static void hist_push(int16_t *buf, int cap, int &pos, const int16_t *src, int n) {
  if (!buf) return;
  for (int i = 0; i < n; i++) { buf[pos] = src[i]; pos = (pos + 1) % cap; }
}
// Fetch `n` samples that were pushed `delaySamples` + n .. `delaySamples` ago.
static void hist_fetch(const int16_t *buf, int cap, int pos, int delaySamples, int16_t *dst, int n) {
  if (!buf) { memset(dst, 0, n * sizeof(int16_t)); return; }
  int start = pos - delaySamples - n;
  while (start < 0) start += cap;
  for (int i = 0; i < n; i++) { dst[i] = buf[start]; start = (start + 1) % cap; }
}
#endif

static void audio_task(void *) {
  static int16_t mic16[FRAME_16K], ref16[FRAME_16K], clean16[FRAME_16K], mic8[FRAME_8K];
  static int16_t spk8[FRAME_8K], spk16[FRAME_16K];
  static int16_t silence16[FRAME_16K] = {0};
#if BOARD_V2
  uint8_t refClip = 0;             // ref frames at full scale in this window
#endif
  int16_t dsHist = 0, usPrev = 0;
  ToneGen tone;
  AudioMode last = AUDIO_IDLE;
  uint32_t echoHold = 0;       // ms of mic attenuation remaining after speaker activity (fallback when AEC is off)
  int clickFrames = 0;
  uint32_t lastStat = 0;
  uint8_t refLevel = 0, rawMicLevel = 0;
  // 2 s window statistics (peak of per-frame peaks)
  uint8_t wSlot[4] = {0, 0, 0, 0}, wMic = 0, wOut = 0, wRef = 0, wSpk = 0;
  uint16_t wFrames = 0, wFarActive = 0, wNearOut = 0;   // frames with speaker active / with AEC output > 5%
  uint16_t wVad = 0;         // times the local VAD reported speech over the AEC output
  uint16_t bargeRun = 0;     // consecutive loud VAD-speech frames (barge-in qualifier)
  bool bargeFired = false;   // one barge-in per speech run
  uint32_t farHold = 0;      // ms since the far end was last active (counts down)
  uint32_t wMaxUs = 0;       // longest frame processing time in the window
  uint16_t wLate = 0;        // frames that took longer than 20 ms
  int64_t frameT0 = 0;
  uint16_t resFrames = 0;    // frames attenuated by the residual suppressor in this window
  uint16_t micFails = 0;     // short I2S captures in this window
  uint16_t wrFails = 0;      // short I2S writes in this window
  uint8_t micFailStreak = 0; // consecutive short captures, for a one-shot warning

  for (;;) {
    AudioMode m = s_mode;
    if (m != last) {
      tone.reset();
      dsHist = usPrev = 0;
      pa_enable(m != AUDIO_IDLE);
      // a prompt never survives a mode change (a call can start mid-chime)
      s_promptSegs = nullptr;
      s_promptBusy = false;
#if BOARD_V2
      // Wake listening runs with the speaker silent, so the microphone can run at
      // full analogue gain; restore the call-tuned PGA as soon as a call starts.
      if (s_codecReady) {
        const uint8_t pga = (m == AUDIO_IDLE) ? DEF_WAKE_MIC_PGA : s_micPga;
        es7210_board_set_gain(1, pga);
        es7210_board_set_gain(2, pga);
      }
#endif
      if (m != AUDIO_IDLE) {
        // prefill TX so the DMA never starts empty
        write_stereo(silence16);
        write_stereo(silence16);
      }
#if BOARD_V2
      if (m == AUDIO_CALL || m == AUDIO_VOICE) {
        memset(s_playHist16, 0, PLAY_HIST_16K * sizeof(int16_t));
        s_playPos16 = 0;
        s_echoGain = 0; s_nearMaxRatio = 0;
        if (s_aecMode != 0) s_aec.start(s_aecMode);
        if (m == AUDIO_VOICE) { s_bargeFlag = false; s_vad.start(); }
        else s_vad.stop();
      } else if (s_aec.active()) {
        s_aec.stop();
        s_vad.stop();
      }
#endif
      last = m;
    }

    if (s_click) { s_click = false; clickFrames = 2; if (m == AUDIO_IDLE) pa_enable(true); }

    // UI prompt tones are rendered here so the PA is otherwise off.
    // (A request that arrives during a call is dropped, not queued.)
    if (s_promptReq != AUDIO_PROMPT_NONE) {
      const uint8_t kind = s_promptReq;
      s_promptReq = AUDIO_PROMPT_NONE;
      if (m == AUDIO_IDLE) {
        s_promptSegs = kPromptWake;
        s_promptSegIdx = 0;
        s_promptSegMs = 0;
        s_promptPhase = 0;
        s_promptBusy = true;
        pa_enable(true);
        Serial.printf("[AUDIO] prompt %u start\n", (unsigned)kind);
      }
    }

    if (m == AUDIO_IDLE) {
      if (s_promptSegs) {
        const bool more = prompt_render(spk16);
        write_stereo(spk16);
        // Keep the capture path running during the chime so the RX DMA ring
        // does not fill with stale audio. In the command stage the samples are
        // still fed: people say "你好小安，呼叫前台" in one breath and the first
        // 300 ms of the command would otherwise be thrown away with the chime.
        // (Waiting for the wake word, the chime's own echo is dropped.)
        read_mic(mic16, ref16);   // keep the RX DMA ring fresh; the chime itself is not fed to WakeNet
        if (!more) {
          s_promptSegs = nullptr;   // stop re-entering the finished prompt
          s_promptBusy = false;
          pa_enable(false);
          Serial.println("[AUDIO] prompt done");
        }
        continue;
      }
      if (clickFrames > 0) {
        tone.fill(spk16, 1200, 0, 1000, 0, 6000);
        write_stereo(spk16);
        if (--clickFrames == 0) { vTaskDelay(pdMS_TO_TICKS(40)); pa_enable(false); tone.reset(); }
        continue;
      }
      s_micLevel = s_spkLevel = 0;
#if WAKE_WORD_ENABLED
      // Idle = wake-word listening. The TDM clocks are driven by the TX channel,
      // so keep writing silence and feed the raw slot-0 microphone to MultiNet.
      write_stereo(silence16);   // also paces the loop (20 ms)
      if (read_mic(mic16, ref16)) WakeWord::feed(mic16, FRAME_16K);
      else vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
      continue;
#else
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
#endif
    }

    // ---- capture (paces the loop) ----
    if (read_mic(mic16, ref16)) {
      if (micFailStreak) {
        Serial.printf("[AUDIO] mic capture recovered after %u short reads\n", (unsigned)micFailStreak);
        micFailStreak = 0;
      }
    } else {
      // A short I2S read means the ADC stopped delivering: count it and say so
      // once when it starts, so a dead capture path is visible in the log.
      micFails++;
      if (++micFailStreak == 5) Serial.println("[AUDIO] mic capture FAILING (I2S short read)");
      vTaskDelay(pdMS_TO_TICKS(FRAME_MS));  // keep pacing even without a mic
    }
#if BOARD_V2
    frameT0 = esp_timer_get_time();
#endif

    if ((m == AUDIO_CALL && s_rtp) || m == AUDIO_VOICE) {
      // ---- uplink ----
      const int16_t *src = mic16;
      bool aecUsed = false;
#if BOARD_V2
      if (s_refSlot == 4) {
        // software reference: what we played s_refDelayMs ago
        hist_fetch(s_playHist16, PLAY_HIST_16K, s_playPos16, (int)s_refDelayMs * AUDIO_HW_RATE / 1000, ref16, FRAME_16K);
      }
      if (s_refDigGain > 1) {
        int32_t g = s_refDigGain;
        for (int i = 0; i < FRAME_16K; i++) ref16[i] = sat16((int32_t)ref16[i] * g);
      }
      refLevel = level_of(ref16, FRAME_16K);
      rawMicLevel = level_of(mic16, FRAME_16K);
      // A reference pinned at full scale is clipped: the AEC then models a
      // nonlinear signal and leaks echo. Watch this column if the model
      // interrupts itself (self-triggered barge-in) or echo survives.
      if (refLevel >= 100) refClip++;
      if (s_aec.active()) {
        if (s_aec.process(mic16, ref16, clean16)) src = clean16;
        else memset(clean16, 0, sizeof(clean16)), src = clean16;  // priming: send silence for the first chunk
        aecUsed = true;
        // Barge-in: the local VAD hears the guest over the echo-cancelled signal
        // and stops the reply without waiting for the server (xiaozhi's flow).
        if (m == AUDIO_VOICE && s_vad.h) {
          // The VAD alone is not enough: at the onset of a reply the AEC has not
          // converged yet and 100-300 ms of residual echo is "speech" to it (the
          // greeting cut itself off after one word). Require the VAD to stay on
          // for BARGE_MIN_MS *and* the cancelled signal to be clearly louder than
          // residual echo during that time. One trigger per speech run.
          s_vad.push(clean16, FRAME_16K);
          const uint8_t outLv = level_of(clean16, FRAME_16K);
          if (s_vad.state == VAD_SPEECH) {
            if (outLv >= BARGE_MIN_LEVEL) bargeRun++;
            if (bargeRun >= BARGE_MIN_MS / FRAME_MS && !bargeFired) { bargeFired = true; s_bargeFlag = true; wVad++; }
          } else {
            bargeRun = 0; bargeFired = false;
          }
        }
        // Residual echo suppression. Only the *quiet* tail of a loud far end is
        // attenuated: the previous "output 9.5 dB below the raw mic" test also
        // caught double-talk (the raw mic is dominated by echo, so real near-end
        // speech after AEC always looked "much quieter") and killed barge-in.
        if (s_resSuppress && farHold > 0 && s_spkLevel > 25) {
          uint8_t outLv = level_of(clean16, FRAME_16K);
          if (outLv < 6) {
            for (int i = 0; i < FRAME_16K; i++) clean16[i] = clean16[i] / 2;   // -6 dB
            resFrames++;
          }
        }
      }
      // Double-talk measurement (see s_echoGain): how much louder than the
      // learned echo path the raw microphone is. No cancellation decision is
      // taken from it.
      if (m == AUDIO_VOICE && s_aec.active() && refLevel > 6) {
        const uint16_t ratio = (uint16_t)((uint32_t)rawMicLevel * 100 / refLevel);
        if (ratio > s_nearMaxRatio) s_nearMaxRatio = ratio > 255 ? 255 : (uint8_t)ratio;
        if (s_echoGain == 0 || ratio < s_echoGain) s_echoGain = ratio;
        else if (s_echoGain < 400) s_echoGain++;
      }
      for (int k = 0; k < 4; k++) if (s_slotLevel[k] > wSlot[k]) wSlot[k] = s_slotLevel[k];
      if (rawMicLevel > wMic) wMic = rawMicLevel;
      if (refLevel > wRef) wRef = refLevel;
      wFrames++;
#endif
      const int32_t g = s_micGain;  // percent
      const bool suppress = !aecUsed && echoHold > 0;
      bool gotFar = false;
      if (m == AUDIO_VOICE) {
        // cloud session: 16 kHz straight through (mic gain / mute applied)
        static int16_t up16[FRAME_16K];
        for (int i = 0; i < FRAME_16K; i++) {
          int32_t v = ((int32_t)src[i] * g) / 100;
          if (suppress) v = v / 16;
          if (s_mute) v = 0;
          up16[i] = sat16(v);
        }
        s_micLevel = level_of(up16, FRAME_16K);
        VoiceSession::pushMic(up16, FRAME_16K);

        // ---- downlink: PCM at the session's output rate -> 16 kHz ----
        const uint32_t rate = VoiceSession::playbackRate();
        if (rate == 24000) {
          static int16_t in24[FRAME_16K * 3 / 2];   // 480 samples = 20 ms
          if (VoiceSession::popPlay(in24, FRAME_16K * 3 / 2) > 0) {
            resample24to16(in24, spk16);
            gotFar = true;
          }
        } else if (rate == 16000) {
          if (VoiceSession::popPlay(spk16, FRAME_16K) > 0) gotFar = true;
        } else if (rate == 8000) {
          if (VoiceSession::popPlay(spk8, FRAME_8K) > 0) { upsample(spk8, spk16, usPrev); gotFar = true; }
        } else if (rate == 48000) {
          static int16_t in48[FRAME_16K * 3];
          if (VoiceSession::popPlay(in48, FRAME_16K * 3) > 0) {
            for (int k = 0; k < FRAME_16K; k++) spk16[k] = (int16_t)(((int32_t)in48[3 * k] + in48[3 * k + 1] + in48[3 * k + 2]) / 3);
            gotFar = true;
          }
        }
        if (gotFar) s_spkLevel = level_of(spk16, FRAME_16K);
      } else {
        downsample(src, mic8, dsHist);
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
          upsample(spk8, spk16, usPrev);
          gotFar = true;
        }
      }
#if BOARD_V2
      if (s_micLevel > wOut) wOut = s_micLevel;
      if (s_micLevel > 5) wNearOut++;
#endif
      if (gotFar) {
#if BOARD_V2
        if (s_spkLevel > wSpk) wSpk = s_spkLevel;
        if (s_spkLevel > 5) wFarActive++;
#endif
        if (s_spkLevel > 6) echoHold = 160; else if (echoHold >= FRAME_MS) echoHold -= FRAME_MS; else echoHold = 0;
        if (s_spkLevel > 4) farHold = 300; else if (farHold >= FRAME_MS) farHold -= FRAME_MS; else farHold = 0;
      } else {
        s_spkLevel = 0;
        if (echoHold >= FRAME_MS) echoHold -= FRAME_MS; else echoHold = 0;
        if (farHold >= FRAME_MS) farHold -= FRAME_MS; else farHold = 0;
        memset(spk16, 0, sizeof(spk16));
        usPrev = 0;
      }
      if (clickFrames > 0) { tone.fill(spk16, 1200, 0, 1000, 0, 5000); clickFrames--; }
#if BOARD_V2
      if (s_refSlot == 4) hist_push(s_playHist16, PLAY_HIST_16K, s_playPos16, spk16, FRAME_16K);
#endif
      if (!write_stereo(spk16)) wrFails++;

#if BOARD_V2
      uint32_t now = millis();
      {
        uint32_t dt = (uint32_t)(esp_timer_get_time() - frameT0);
        if (dt > wMaxUs) wMaxUs = dt;
        if (dt > 20000) wLate++;
      }
      if (now - lastStat > 2000) {
        lastStat = now;
        if (m == AUDIO_VOICE) {
          VoiceSession::Stats vs = VoiceSession::stats();
          Serial.printf("[VOICE] aec %lu us frame max %lu us late=%u | mic=%u out=%u ref=%u spk=%u far=%u%% res=%u%% refclip=%u%% g=%u r=%u vad=%u | tx=%lu rxAudio=%lu depth=%dms under=%lu drop=%lu micdrop=%lu heap=%u\n",
                        (unsigned long)(s_aec.procCount ? s_aec.procUs / s_aec.procCount : 0), (unsigned long)wMaxUs, wLate,
                        wMic, wOut, wRef, wSpk, wFrames ? wFarActive * 100 / wFrames : 0, wFrames ? resFrames * 100 / wFrames : 0,
                        wFrames ? refClip * 100 / wFrames : 0, (unsigned)s_echoGain, (unsigned)s_nearMaxRatio, (unsigned)wVad,
                        (unsigned long)vs.txChunks, (unsigned long)vs.rxAudioBytes, vs.playDepthMs, (unsigned long)vs.playUnderruns,
                        (unsigned long)vs.playDrops, (unsigned long)vs.micDrops, (unsigned)ESP.getFreeHeap());
          s_aec.procUs = 0; s_aec.procCount = 0; s_aec.procMaxUs = 0;
        } else if (s_aec.active() && s_aec.procCount) {
          Serial.printf("[AEC] %lu/%lu us frame max %lu us late=%u | mic=%u out=%u ref=%u spk=%u far=%u%% out>5=%u%% res=%u%% micfail=%u wrfail=%u | rtp rx=%lu tx=%lu txfail=%lu gaps=%lu under=%lu drop=%lu depth=%d\n",
                        (unsigned long)(s_aec.procUs / s_aec.procCount), (unsigned long)s_aec.procMaxUs,
                        (unsigned long)wMaxUs, wLate, wMic, wOut, wRef, wSpk,
                        wFrames ? wFarActive * 100 / wFrames : 0, wFrames ? wNearOut * 100 / wFrames : 0,
                        wFrames ? resFrames * 100 / wFrames : 0, (unsigned)micFails, (unsigned)wrFails,
                        (unsigned long)s_rtp->statRx(), (unsigned long)s_rtp->statTx(), (unsigned long)s_rtp->statTxFail(),
                        (unsigned long)s_rtp->statSeqGaps(), (unsigned long)s_rtp->statUnderruns(),
                        (unsigned long)s_rtp->statDrops(), s_rtp->jbDepth());
          s_rtp->statsReset();
          s_aec.procUs = 0; s_aec.procCount = 0; s_aec.procMaxUs = 0;
        } else {
          Serial.printf("[AUDIO] peak2s slots=[%u %u %u %u] mic(s%u)=%u out=%u ref=%u spk=%u far=%u%% out>5=%u%% refclip=%u%% micfail=%u rtp tx=%lu txfail=%lu (aec off)\n",
                        wSlot[0], wSlot[1], wSlot[2], wSlot[3], s_micSlot, wMic, wOut, wRef, wSpk,
                        wFrames ? wFarActive * 100 / wFrames : 0, wFrames ? wNearOut * 100 / wFrames : 0,
                        wFrames ? refClip * 100 / wFrames : 0,
                        (unsigned)micFails, (unsigned long)s_rtp->statTx(), (unsigned long)s_rtp->statTxFail());
          s_rtp->statsReset();
        }
        memset(wSlot, 0, sizeof(wSlot)); wMic = wOut = wRef = wSpk = 0; wFrames = wFarActive = wNearOut = 0; resFrames = 0; wMaxUs = 0; wLate = 0;
        micFails = 0; wrFails = 0; refClip = 0; s_nearMaxRatio = 0; wVad = 0;
      }
#endif
      continue;
    }

    // ---- local tones ----
    switch (m) {
      case AUDIO_RINGBACK: tone.fill(spk16, 450, 0, 1000, 4000, 7000); break;          // CN ringback
      case AUDIO_RINGING:  tone.fill(spk16, 880, 1047, 1000, 2000, 9000); break;        // two-tone ring
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
#if BOARD_V2
  if (!hist_alloc()) Serial.println("[AUDIO] history alloc failed");
#endif
  if (s_task) return;
  // Core 1: away from the WiFi/lwIP stack on core 0. LVGL (loop()) also runs on
  // core 1 but at a lower priority, so the 20 ms audio cadence is never starved.
  xTaskCreatePinnedToCore(audio_task, "audio", 12288, NULL, 12, &s_task, 1);
}
