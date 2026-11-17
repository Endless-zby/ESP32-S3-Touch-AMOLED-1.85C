// ============================================================================
//  AICC Phone - SIP telephone for Waveshare ESP32-S3-Touch-LCD-1.85C
//  Calls the aicc (ai-native-callcenter) FreeSWITCH over SIP/RTP (G.711).
// ============================================================================
#include <Arduino.h>
#include <WiFi.h>
#include "config.h"
#include "settings.h"
#include "board_io.h"
#include "lvgl_port.h"
#include "touch.h"
#include "ui.h"
#include "ui_strings.h"
#include "audio.h"
#include "wake_word.h"
#include "assistant.h"
#include "face_model.h"
#include "rtp.h"
#include "sip.h"
#include "web_portal.h"

static SipClient g_sip;
static RtpSession g_rtp;

// Voice assistant: "你好小安" (local WakeNet) wakes the device, then a Qwen
// Realtime session carries the conversation and drives volume / brightness /
// front-desk calls through function calling (see assistant.cpp).

enum NetState { NET_IDLE, NET_CONNECTING, NET_CONNECTED, NET_SETUP };
static NetState g_net = NET_IDLE;
static uint32_t g_netSince = 0;
static uint32_t g_apStopAt = 0;
static bool g_portalWifiAttempt = false;   // connection attempt triggered from the web page
static bool g_sipStarted = false;
static bool g_audioOk = false;

static uint32_t g_callConnectedAt = 0;
static uint32_t g_endedAt = 0;              // when the "call ended" screen was shown
static uint32_t g_lastRtpCheck = 0;
static uint32_t g_lastUiTick = 0;
static uint32_t g_settingsSaveAt = 0;   // debounced NVS write (voice volume commands)

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static const char *sipStatusText(uint32_t &color) {
  if (WiFi.status() != WL_CONNECTED) { color = 0xEF4444; return STR_NET_DOWN; }
  if (!g_settings.sipRegister) { color = 0x22C55E; return STR_SIP_DIRECT; }
  switch (g_sip.regState()) {
    case SIP_REG_OK: color = 0x22C55E; return STR_SIP_REGISTERED;
    case SIP_REG_TRYING: color = 0xF59E0B; return STR_SIP_REGISTERING;
    case SIP_REG_FAILED: color = 0xEF4444; return STR_SIP_REG_FAILED;
    default: color = 0x8A93A6; return STR_SIP_DIRECT;
  }
}

static void refreshNetStatus() {
  uint32_t color;
  const char *t = sipStatusText(color);
  ui_set_net_status(WiFi.status() == WL_CONNECTED, t, color);
}

static const char *endReasonText(const String &status) {
  if (status.startsWith("486") || status.startsWith("600")) return STR_BUSY;
  if (status.startsWith("404")) return STR_NOT_FOUND;
  if (status.startsWith("480") || status.startsWith("408") || status == "Timeout") return STR_TIMEOUT;
  if (status.startsWith("603") || status == "Rejected") return STR_REJECTED;
  if (status == "Cancelled") return STR_CANCELLED;
  if (status == "Remote hung up") return STR_REMOTE_HANGUP;
  if (status == "Missed") return STR_MISSED;
  if (status == "Ended" || status.length() == 0) return STR_ENDED;
  if (status[0] >= '3' && status[0] <= '6') return STR_CALL_FAILED;
  return STR_ENDED;
}

static void startSip() {
  SipConfig c;
  c.server = g_settings.sipServer;
  c.port = g_settings.sipPort;
  c.user = g_settings.sipUser;
  c.password = g_settings.sipPass;
  c.displayName = g_settings.displayName;
  c.doRegister = g_settings.sipRegister;
  c.localPort = SIP_LOCAL_PORT;
  c.rtpPort = RTP_LOCAL_PORT;
  g_rtp.end();
  g_rtp.begin(RTP_LOCAL_PORT);
  g_sipStarted = g_sip.begin(c);
}

// ---------------------------------------------------------------------------
// SIP callbacks (run in loop() context)
// ---------------------------------------------------------------------------
static void onCallState(SipCallState s) {
  switch (s) {
    case SIP_CALL_INVITING:
      assistant_on_call_started();
      ui_show_call(g_sip.remoteParty().c_str(), STR_CALLING);
      audio_set_mode(AUDIO_IDLE);
      break;
    case SIP_CALL_RINGING:
      ui_call_status(STR_RINGING_OUT);
      if (audio_mode() != AUDIO_CALL) audio_set_mode(AUDIO_RINGBACK);
      break;
    case SIP_CALL_INCOMING:
      assistant_on_call_started();
      ui_show_incoming(g_sip.remoteParty().c_str());
      audio_set_mode(AUDIO_RINGING);
      break;
    case SIP_CALL_CONNECTED:
      g_callConnectedAt = millis();
      ui_show_call(g_sip.remoteParty().c_str(), STR_CONNECTED);   // stay on the call page
      break;
    case SIP_CALL_ENDING:
      ui_call_status(STR_ENDING);
      audio_set_mode(AUDIO_IDLE);
      break;
    case SIP_CALL_IDLE: {
      audio_set_mode(AUDIO_IDLE);
      audio_set_mute(false);
      g_rtp.clearRemote();
      const char *why = endReasonText(g_sip.lastStatus());
      ui_show_face();
      ui_face_call_active(false);
      if (ui_screen() == UI_INCOMING) ui_show_call(g_sip.remoteParty().c_str(), why);
      else ui_toast(why);
      ui_call_timer(0, false);
      g_endedAt = millis();
      g_callConnectedAt = 0;
      Serial.printf("[APP] call ended: %s\n", g_sip.lastStatus().c_str());
      assistant_on_call_ended();
      break;
    }
  }
}

static void onRegState(SipRegState) { refreshNetStatus(); }

static void onMediaStart(const SipMedia &m) {
  Serial.printf("[APP] media -> %s:%u pt=%u dtmf=%d\n", m.ip.toString().c_str(), m.port, m.pt, m.dtmfPt);
  g_rtp.setRemote(m.ip, m.port, m.pt, m.dtmfPt);
  audio_set_mode(AUDIO_CALL);
}

static void onMediaStop() {
  audio_set_mode(AUDIO_IDLE);
  g_rtp.clearRemote();
}

// ---------------------------------------------------------------------------
// UI callbacks
// ---------------------------------------------------------------------------
static void uiDial(const char *number) {
  if (WiFi.status() != WL_CONNECTED || !g_sipStarted) { ui_toast(STR_NO_NETWORK); return; }
  if (!g_sip.isIdle()) return;
  if (!g_sip.call(String(number))) ui_toast(STR_CALL_FAILED);
}
static void uiHangup() { g_sip.hangup(); }
static void uiAnswer() { g_sip.answer(); }
static void uiReject() { g_sip.reject(); }
static void uiMute(bool m) { audio_set_mute(m); ui_toast(m ? STR_MUTED : STR_UNMUTED, 1000); }
static void uiVolume(int delta) {
  int v = (int)g_settings.volume + delta;
  v = constrain(v, 0, 100);
  g_settings.volume = v;
  audio_set_volume(v);
  ui_set_volume(v);
  g_settings.save();
}
static void uiResetWifi() {
  ui_toast(STR_RESTARTING, 3000);
  g_settings.clearWifi();
  delay(300);
  ESP.restart();
}
static void uiKey() { audio_click(); }
static void showSettings() {
  uint32_t c;
  String srv = g_settings.sipServer + ":" + String(g_settings.sipPort);
  ui_show_settings(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "-", srv.c_str(),
                   sipStatusText(c), web_ap_ssid().c_str(), FW_VERSION);
}

// ---------------------------------------------------------------------------
// WiFi handling
// ---------------------------------------------------------------------------
static void beginSta(const String &ssid, const String &pass) {
  Serial.printf("[NET] connecting to '%s'\n", ssid.c_str());
  WiFi.setSleep(false);
  WiFi.setHostname("aicc-phone");
  WiFi.begin(ssid.c_str(), pass.c_str());
  g_net = NET_CONNECTING;
  g_netSince = millis();
}

static void enterSetupMode() {
  web_start_ap();
  g_net = NET_SETUP;
  ui_show_setup(web_ap_ssid().c_str(), AP_PASSWORD, "192.168.4.1");
  web_set_status("等待配置", "", false);
}

static void onWifiConnected() {
  Serial.printf("[NET] connected, ip=%s\n", WiFi.localIP().toString().c_str());
  g_net = NET_CONNECTED;
  web_begin();
  web_set_status("连接成功", WiFi.localIP().toString(), true);
  // Persist whatever credentials got us online (covers a portal attempt that
  // briefly looked failed before succeeding).
  if (WiFi.SSID() == g_settings.wifiSsid || g_settings.wifiSsid.length()) g_settings.save();
  if (g_portalWifiAttempt || web_ap_active()) {
    g_portalWifiAttempt = false;
    g_apStopAt = millis() + 45000;   // keep the AP a little longer so the phone sees the result
  }
  startSip();
  ui_set_default_number(g_settings.dialNumber.c_str());
  static bool ntpStarted = false;
  if (!ntpStarted) {
    ntpStarted = true;
    configTime(8 * 3600, 0, "ntp.aliyun.com", "ntp1.aliyun.com", "pool.ntp.org");
  }
  ui_show_face();
  refreshNetStatus();
  if (g_apStopAt) ui_toast(STR_SETUP_OK, 2500);
}

static void netLoop() {
  uint32_t now = millis();
  String ssid, pass;
  if (web_take_wifi_request(ssid, pass)) {
    g_settings.wifiSsid = ssid;
    g_settings.wifiPass = pass;
    g_portalWifiAttempt = true;
    WiFi.disconnect(false, false);
    delay(100);
    beginSta(ssid, pass);
    if (ui_screen() == UI_SETUP) {
      String t = String(STR_SETUP_TRYING) + " " + ssid + " ...";
      ui_setup_status(t.c_str());
    }
    web_set_status("正在连接", "", false);
  }
  if (web_take_restart_request()) {
    ui_toast(STR_RESTARTING, 3000);
    delay(800);
    ESP.restart();
  }
  if (web_take_wifi_reset_request()) {
    g_settings.clearWifi();
    delay(300);
    ESP.restart();
  }

  wl_status_t st = WiFi.status();
  switch (g_net) {
    case NET_CONNECTING:
      if (st == WL_CONNECTED) {
        onWifiConnected();
      } else if (now - g_netSince > 25000 || (st == WL_CONNECT_FAILED && now - g_netSince > 8000)) {
        Serial.println("[NET] connect failed");
        if (g_portalWifiAttempt) {
          g_portalWifiAttempt = false;
          web_set_status("连接失败，请检查密码", "", false);
          ui_setup_status(STR_SETUP_FAIL);
          g_net = NET_SETUP;
          if (!web_ap_active()) enterSetupMode();
        } else if (web_ap_active()) {
          g_net = NET_SETUP;
          ui_setup_status(STR_SETUP_FAIL);
        } else {
          enterSetupMode();
          ui_setup_status(STR_WIFI_FAILED);
          g_netSince = now;
        }
      }
      break;
    case NET_SETUP:
      // keep retrying the stored network in the background every 30 s
      if (g_settings.hasWifi() && !g_portalWifiAttempt && now - g_netSince > 30000) {
        g_netSince = now;
        WiFi.begin(g_settings.wifiSsid.c_str(), g_settings.wifiPass.c_str());
      }
      if (st == WL_CONNECTED) onWifiConnected();
      break;
    case NET_CONNECTED:
      if (st != WL_CONNECTED) {
        Serial.println("[NET] link lost, reconnecting");
        g_sip.end();
        g_sipStarted = false;
        if (!g_sip.isIdle()) g_sip.hangup();
        audio_set_mode(AUDIO_IDLE);
        refreshNetStatus();
        WiFi.reconnect();
        g_net = NET_CONNECTING;
        g_netSince = now;
      }
      if (g_apStopAt && (int32_t)(now - g_apStopAt) >= 0) {
        g_apStopAt = 0;
        web_stop_ap();
      }
      break;
    default: break;
  }
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n[APP] AICC Phone %s (board %s) reset=%d\n", FW_VERSION, BOARD_V2 ? "V2" : "V1", (int)esp_reset_reason());

  g_settings.load();
  board_i2c_init();
  exio_init(0x00);
  backlight_init();
  battery_init();

  if (!lvgl_port_init()) Serial.println("[APP] display init FAILED");
  touch_init();

  UiCallbacks cb;
  cb.onDial = uiDial;
  cb.onHangup = uiHangup;
  cb.onAnswer = uiAnswer;
  cb.onReject = uiReject;
  cb.onMute = uiMute;
  cb.onVolume = uiVolume;
  cb.onResetWifi = uiResetWifi;
  cb.onKey = uiKey;
  cb.onSettings = showSettings;
  ui_init(cb);
  ui_set_default_number(g_settings.dialNumber.c_str());
  ui_refresh_now();
  backlight_set(80);

  ui_boot_status(STR_INIT_AUDIO);
  ui_refresh_now();
  audio_set_ref_gain(g_settings.refGain);   // before init: picked up by es7210_board_init()
  audio_set_mic_pga(g_settings.micPga);
  audio_set_ref_dig_gain(g_settings.refDigGain);
  audio_set_res_suppress(g_settings.resSuppress);
  g_audioOk = audio_init();
  audio_set_volume(g_settings.volume);
  audio_set_mic_gain(g_settings.micGain);
  audio_set_aec_mode(g_settings.aecMode);
  audio_set_slots(g_settings.micSlot, g_settings.refSlot);
  audio_set_ref_delay(g_settings.refDelayMs);
  audio_set_aec_filter(g_settings.aecFilter);
  audio_start_task(&g_rtp);

  // Wake word (ESP-SR WakeNet9, model pack in the "model" partition) + assistant
  WakeWord::begin();
  WakeWord::setSensitivity(g_settings.wakeSens);
  {
    AssistantHooks h;
    h.setVolume = [](int level) {
      g_settings.volume = constrain(level, 0, 100);
      audio_set_volume(g_settings.volume);
      ui_set_volume(g_settings.volume);
      g_settingsSaveAt = millis() + 3000;   // persist once, when the guest stops adjusting
    };
    h.getVolume = []() -> int { return g_settings.volume; };
    h.setBrightness = [](int level) { ui_brightness_adjust(level - ui_brightness()); };
    h.getBrightness = []() -> int { return ui_brightness(); };
    h.dialFrontDesk = []() -> bool {
      if (WiFi.status() != WL_CONNECTED || !g_sipStarted || !g_sip.isIdle()) return false;
      return g_sip.call(g_settings.dialNumber);
    };
    h.phoneBusy = []() -> bool { return !g_sip.isIdle(); };
    assistant_begin(h);
  }
  pinMode(0, INPUT_PULLUP);   // BOOT button (also the ESP32-S3 boot strap)

  g_sip.onCallState = onCallState;
  g_sip.onRegState = onRegState;
  g_sip.onMediaStart = onMediaStart;
  g_sip.onMediaStop = onMediaStop;

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  if (g_settings.hasWifi()) {
    ui_boot_status(STR_WIFI_CONNECTING);
    ui_refresh_now();
    beginSta(g_settings.wifiSsid, g_settings.wifiPass);
  } else {
    enterSetupMode();
  }
}

void loop() {
  lvgl_port_loop();
  web_loop();
  if (g_sipStarted) g_sip.loop();
  netLoop();

  // Wake word -> assistant
  if (WakeWord::consume()) assistant_on_wake_word();
  assistant_loop();

  // Not registered to FreeSWITCH: while the device is asleep there is nothing to
  // keep alive, so stop the OPTIONS probe and the NAT keepalive. Waking turns it
  // back on and refreshes the public address before the guest can dial.
  {
    static bool hbOn = true;
    const bool sleeping = assistant_state() == AS_SLEEP && ui_screen_asleep();
    const bool want = !(sleeping && !g_settings.sipRegister);
    if (want != hbOn) { hbOn = want; g_sip.setHeartbeat(want); }
  }

  // BOOT (GPIO0, active low): open/close the settings page.
  {
    static bool bootWas = false;
    static uint32_t bootAt = 0;
    const bool bootNow = (digitalRead(0) == LOW);
    const uint32_t bn = millis();
    if (bootNow && !bootWas && bn - bootAt > 250) {
      bootAt = bn;
      audio_click();
      if (ui_screen_asleep()) { assistant_on_user_wake(); }
      else if (ui_screen() == UI_SETTINGS || ui_screen() == UI_INFO) ui_settings_back();
      else if (ui_screen() == UI_DIALER) ui_show_face();
      else showSettings();
    }
    bootWas = bootNow;
  }

  uint32_t now = millis();

  if (g_settingsSaveAt && (int32_t)(now - g_settingsSaveAt) >= 0) {
    g_settingsSaveAt = 0;
    g_settings.save();
  }

  // Touch while asleep wakes the screen and starts a conversation; touch while
  // awake just postpones the screen-off.
  {
    static uint32_t lastTouchPoll = 0;
    if (now - lastTouchPoll > 100) {
      lastTouchPoll = now;
      uint16_t tx, ty;
      if (touch_read(tx, ty)) {
        if (ui_screen_asleep()) assistant_on_user_wake();
        else assistant_touch_activity();
      }
    }
  }

  // Wake-word engine health every 60 s: CPU per chunk and counters.
  {
    static uint32_t lastWakeStat = 0;
    if (now - lastWakeStat > 60000) {
      lastWakeStat = now;
      WakeWord::Stats ws = WakeWord::stats();
      Serial.printf("[WAKE] assistant=%d listening=%d wakes=%lu wn avg/max %lu/%lu us %s\n",
                    (int)assistant_state(), (int)ws.listening, (unsigned long)ws.wakes,
                    (unsigned long)ws.avgUs, (unsigned long)ws.maxUs, ws.ready ? "" : ws.error);
    }
  }

  // call timer / meters (4x per second)
  if (now - g_lastUiTick >= 250) {
    g_lastUiTick = now;
    if (ui_screen() == UI_CALL && g_callConnectedAt) ui_call_timer((now - g_callConnectedAt) / 1000, true);
    if (ui_screen() == UI_FACE && assistant_state() == AS_CALL) ui_face_levels(audio_mic_level(), audio_spk_level());
    // return to the dialer a moment after a call has ended
    if (g_endedAt && g_sip.isIdle() && now - g_endedAt > 2500 && (ui_screen() == UI_CALL || ui_screen() == UI_INCOMING)) {
      g_endedAt = 0;
      ui_show_face();
      refreshNetStatus();
    }
    if (ui_screen() == UI_SETTINGS || ui_screen() == UI_INFO) {
      static uint32_t lastSet = 0;
      if (now - lastSet > 2000) { lastSet = now; showSettings(); }
    }
  }

  static uint32_t lastHeapLog = 0;
  if (now - lastHeapLog > 30000) {
    lastHeapLog = now;
    Serial.printf("[APP] heap free=%u min=%u psram=%u rssi=%d\n", (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getFreePsram(),
                WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  }

  // media watchdog: no RTP for 15 s during a connected call -> hang up
  if (g_sip.callState() == SIP_CALL_CONNECTED && now - g_lastRtpCheck > 1000) {
    g_lastRtpCheck = now;
    // lastRxMs is written by the audio task on the other core and can be a
    // millisecond ahead of this loop's `now`; age it with a signed delta.
    uint32_t last = g_rtp.lastRxMs();
    int32_t since = (int32_t)(now - (last ? last : g_callConnectedAt));
    if (since > 15000) {
      Serial.println("[APP] RTP timeout -> hangup");
      g_sip.hangup();
    }
  }

  delay(3);
}
