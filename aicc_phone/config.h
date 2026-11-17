#pragma once
// ============================================================================
//  AICC Phone - board / project configuration
//  Target: Waveshare ESP32-S3-Touch-LCD-1.85C (round 360x360)
// ============================================================================

// ---- Board revision -------------------------------------------------------
// 1 = Rev2.0 (ES8311 codec + ES7210 dual mic ADC + NS4150B PA)
// 0 = V1     (PCM5101 DAC + MSM261 digital MEMS mic)   -- untested path
#ifndef BOARD_V2
#define BOARD_V2 1
#endif

// ---- I2C (shared: TCA9554 / CST816 / PCF85063 / codecs) -------------------
#define PIN_I2C_SDA 11
#define PIN_I2C_SCL 10

// ---- LCD ST77916 over QSPI -------------------------------------------------
#define PIN_LCD_CS  21
#define PIN_LCD_SCK 40
#define PIN_LCD_D0  46
#define PIN_LCD_D1  45
#define PIN_LCD_D2  42
#define PIN_LCD_D3  41
#define PIN_LCD_TE  18
#define PIN_LCD_BL  5
#define LCD_W 360
#define LCD_H 360
#define LCD_QSPI_HZ (80 * 1000 * 1000)

// ---- Touch CST816 ----------------------------------------------------------
#define PIN_TP_INT 4

// ---- TCA9554 expander pins (1-based, as in Waveshare demo) -----------------
#define EXIO_TP_RST  1
#define EXIO_LCD_RST 2

// ---- Battery ---------------------------------------------------------------
#define PIN_BAT_ADC 8

// ---- Audio -----------------------------------------------------------------
#if BOARD_V2
#define PIN_I2S_MCLK 2
#define PIN_I2S_BCLK 48
#define PIN_I2S_LRCK 38
#define PIN_I2S_DOUT 47
#define PIN_I2S_DIN  39
#define PIN_PA_CTRL  15
#else
#define PIN_SPK_BCLK 48
#define PIN_SPK_LRCK 38
#define PIN_SPK_DOUT 47
#define PIN_MIC_BCLK 15
#define PIN_MIC_WS   2
#define PIN_MIC_DIN  39
#endif

#define AUDIO_HW_RATE 16000     // codec sample rate
#define RTP_RATE      8000      // G.711
#define FRAME_MS      20
#define FRAME_8K      (RTP_RATE * FRAME_MS / 1000)       // 160
#define FRAME_16K     (AUDIO_HW_RATE * FRAME_MS / 1000)  // 320

// ---- Defaults (editable at runtime through the web page) -------------------
#define DEF_SIP_SERVER   "113.249.109.160"
#define DEF_SIP_PORT     5080
#define DEF_DIAL_NUMBER  "8888"
#define DEF_SIP_USER     "esp32phone"
#define DEF_SIP_PASS     ""
#define DEF_DISPLAY_NAME "ESP32 Phone"
#define DEF_SIP_REGISTER 0
#define DEF_VOLUME       55
#define DEF_MIC_GAIN     150     // digital mic gain (%) applied after AEC
#define DEF_AEC_MODE     1       // 0 off, 1 esp-sr FD low cost (xiaozhi default), 2 VoIP high perf, 3 FD high perf
#define DEF_MIC_SLOT     0       // V2: ES7210 TDM slot used as the voice microphone
#define DEF_REF_SLOT     1       // V2: ES7210 TDM slot with the DAC loopback (MIC3), 4 = software reference
#define DEF_REF_DELAY_MS 40      // software reference delay (tune with the lag= value in the serial log)
#define DEF_AEC_FILTER   4       // esp-sr AEC filter_length (echo tail ~ n x 16 ms)
#define DEF_REF_GAIN     14      // ES7210 MIC3 PGA gain code (14 = 37.5 dB; the on-board attenuator is ~-24 dB)
#define DEF_MIC_PGA      6       // ES7210 MIC1/MIC2 PGA gain code (6 = 18 dB). 30 dB clipped on speaker echo.
#define DEF_REF_DIG_GAIN 4       // software gain on the reference channel before the AEC (x1..x8)
#define DEF_RES_SUPPRESS 1       // residual echo suppression after the AEC while the far end is talking
// ---- 唤醒词（ESP-SR WakeNet9，本地）------------------------------------------
// 模型包在 flash 的 "model" 分区（tools/srmodels/srmodels_cn.bin，由 tools/pack_models.sh 打包）：
//   wn9_nihaoxiaoan_tts2  唤醒词 "你好小安"（乐鑫免费商用模型，常开）
// 唤醒后的对话、音量/亮度/呼叫前台全部由云端 Qwen Realtime 的 function calling 完成（assistant.cpp）。
#define WAKE_WORD_ENABLED   1
#define WAKE_WORD_TEXT      "你好小安"   // WakeNet9 wn9_nihaoxiaoan_tts2 (tools/srmodels/src)
#define DEF_WAKE_SENSITIVITY 0       // 0 标准(模型默认阈值) 1 宽松(-0.05) 2 严格(+0.01)
#define DEF_WAKE_MIC_PGA    10       // 待机监听时 ES7210 PGA code（10 = 30 dB，喇叭安静无削波风险）
#define AUDIO_CFG_VERSION 4      // bump to reset the audio tuning fields stored in NVS
#define SIP_LOCAL_PORT   5060
#define RTP_LOCAL_PORT   20000

// ---- Qwen Realtime voice assistant (device connects to DashScope directly) ----
#define DEF_AI_HOST          "dashscope.aliyuncs.com"       // or <WorkspaceId>.cn-beijing.maas.aliyuncs.com
#define DEF_AI_MODEL         "qwen-audio-3.1-realtime-flash"
#define DEF_AI_VOICE         ""                              // empty = server default (longanqian_v3.1)
#define DEF_AI_OUT_RATE      24000                           // PCM rate of response.audio.delta
#define DEF_AI_IDLE_SEC      120                             // no speech from either side -> sleep
#define DEF_AI_EXTRA         ""                              // hotel-specific facts appended to the prompt
#define VOICE_TURN_DETECTION "smart_turn_v2"
#define VOICE_CONNECT_TIMEOUT_MS 15000
#define VOICE_PREROLL_MS     240

#define AP_SSID_PREFIX "AICC-Phone-"
#define AP_PASSWORD    "12345678"
#define FW_VERSION     "1.0.0"
