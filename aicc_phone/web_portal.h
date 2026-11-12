#pragma once
#include <Arduino.h>

// Captive-portal style provisioning + settings page.
void web_begin();                 // start HTTP server (any mode)
void web_start_ap();              // start SoftAP + DNS redirect
void web_stop_ap();
bool web_ap_active();
String web_ap_ssid();
void web_loop();

// Results produced by the page, consumed by the application loop
bool web_take_wifi_request(String &ssid, String &pass);   // user submitted new WiFi creds
bool web_take_restart_request();                          // settings saved -> restart
bool web_take_wifi_reset_request();
// Application reports connection status back for the page to show
void web_set_status(const char *stateText, const String &ip, bool connected);
