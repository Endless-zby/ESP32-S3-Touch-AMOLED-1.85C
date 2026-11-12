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
  uint8_t micSlot;     // V2: TDM slot of the voice mic (0..3)
  uint8_t refSlot;     // V2: 0..3 TDM slot, 4 = software reference
  uint16_t refDelayMs; // software reference delay
  uint8_t aecFilter;   // esp-sr filter_length
  uint8_t refGain;     // ES7210 reference PGA gain code
  uint8_t micPga;      // ES7210 MIC1/2 PGA gain code
  uint8_t refDigGain;  // software reference gain x1..x8
  bool resSuppress;    // residual echo suppression

  void load();
  void save();
  void clearWifi();
  bool hasWifi() const { return wifiSsid.length() > 0; }
};

extern Settings g_settings;
