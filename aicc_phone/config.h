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
#define DEF_VOLUME       75
#define DEF_MIC_GAIN     100
#define DEF_AEC_MODE     1       // 0 off, 1 esp-sr FD low cost (xiaozhi default), 2 VoIP high perf, 3 FD high perf
#define DEF_MIC_SLOT     0       // V2: ES7210 TDM slot used as the voice microphone
#define DEF_REF_SLOT     1       // V2: ES7210 TDM slot with the DAC loopback (MIC3), 4 = software reference
#define DEF_REF_DELAY_MS 40      // software reference delay (tune with the lag= value in the serial log)
#define DEF_AEC_FILTER   4       // esp-sr AEC filter_length (echo tail ~ n x 16 ms)
#define DEF_REF_GAIN     14      // ES7210 MIC3 PGA gain code (14 = 37.5 dB; the on-board attenuator is ~-24 dB)
#define AUDIO_CFG_VERSION 3      // bump to reset the audio tuning fields stored in NVS
#define SIP_LOCAL_PORT   5060
#define RTP_LOCAL_PORT   20000

#define AP_SSID_PREFIX "AICC-Phone-"
#define AP_PASSWORD    "12345678"
#define FW_VERSION     "1.0.0"
