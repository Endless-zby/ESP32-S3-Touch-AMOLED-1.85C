#pragma once
#include <Arduino.h>

struct Settings {
  String wifiSsid;
  String wifiPass;
  String sipServer;
  uint16_t sipPort;
  String dialNumber;   // number dialed by the big green button
  String sipUser;      // local user part / auth username
  String sipPass;      // auth password (only needed when registering)
  String displayName;
  bool sipRegister;
  uint8_t volume;      // 0..100
  uint8_t micGain;     // 0..200 (%)
  uint8_t aecMode;     // 0 off, 1 low cost, 2 high performance

  void load();
  void save();
  void clearWifi();
  bool hasWifi() const { return wifiSsid.length() > 0; }
};

extern Settings g_settings;
