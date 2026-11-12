#pragma once
#include <Arduino.h>
#include <lvgl.h>

struct UiCallbacks {
  void (*onDial)(const char *number) = nullptr;
  void (*onHangup)() = nullptr;
  void (*onAnswer)() = nullptr;
  void (*onReject)() = nullptr;
  void (*onMute)(bool muted) = nullptr;
  void (*onVolume)(int delta) = nullptr;   // +/- step
  void (*onResetWifi)() = nullptr;
  void (*onKey)() = nullptr;               // key click feedback
  void (*onSettings)() = nullptr;          // gear button pressed
};

enum UiScreen { UI_BOOT, UI_SETUP, UI_FACE, UI_DIALER, UI_CALL, UI_INCOMING, UI_SETTINGS, UI_INFO };

void ui_init(const UiCallbacks &cb);
UiScreen ui_screen();

// Face home screen (idle + in-call). Expression ids come from face_model.h ExprId.
void ui_show_face();
void ui_face_set_expr(int exprId);
void ui_face_call_active(bool active);   // show the hang-up button; tapping the face hangs up
void ui_face_levels(uint8_t mic, uint8_t spk);   // listening / speaking expressions
void ui_face_listening(bool on);
void ui_settings_back();   // info -> menu, menu -> face   // purple breathing ring: wake word heard, waiting for a command

void ui_show_boot(const char *status);
void ui_boot_status(const char *status);

void ui_show_setup(const char *apSsid, const char *apPass, const char *url);
void ui_setup_status(const char *status);

void ui_show_dialer();
void ui_set_default_number(const char *number);
void ui_set_net_status(bool wifiOk, const char *sipText, uint32_t colorHex);

void ui_show_call(const char *party, const char *status);
void ui_call_status(const char *status);
void ui_call_timer(uint32_t seconds, bool show);
void ui_call_levels(uint8_t mic, uint8_t spk);
void ui_set_muted(bool muted);
void ui_set_volume(int percent);

void ui_show_incoming(const char *caller);

void ui_show_settings(const char *ip, const char *server, const char *sipState, const char *apName, const char *version);

void ui_toast(const char *msg, uint32_t ms = 2000);
void ui_refresh_now();   // force an immediate redraw (used during boot before loop() runs)
