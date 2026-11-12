#include "settings.h"
#include "config.h"
#include <Preferences.h>

Settings g_settings;
static const char *NS = "aiccphone";

void Settings::load() {
  Preferences p;
  p.begin(NS, true);
  wifiSsid = p.getString("wssid", "");
  wifiPass = p.getString("wpass", "");
  sipServer = p.getString("sserver", DEF_SIP_SERVER);
  sipPort = p.getUShort("sport", DEF_SIP_PORT);
  dialNumber = p.getString("dial", DEF_DIAL_NUMBER);
  sipUser = p.getString("suser", DEF_SIP_USER);
  sipPass = p.getString("spass", DEF_SIP_PASS);
  displayName = p.getString("dname", DEF_DISPLAY_NAME);
  sipRegister = p.getBool("sreg", DEF_SIP_REGISTER);
  volume = p.getUChar("vol", DEF_VOLUME);
  micGain = p.getUChar("mic", DEF_MIC_GAIN);
  p.end();
  if (sipServer.length() == 0) sipServer = DEF_SIP_SERVER;
  if (sipPort == 0) sipPort = DEF_SIP_PORT;
  if (sipUser.length() == 0) sipUser = DEF_SIP_USER;
  if (volume > 100) volume = 100;
}

void Settings::save() {
  Preferences p;
  p.begin(NS, false);
  p.putString("wssid", wifiSsid);
  p.putString("wpass", wifiPass);
  p.putString("sserver", sipServer);
  p.putUShort("sport", sipPort);
  p.putString("dial", dialNumber);
  p.putString("suser", sipUser);
  p.putString("spass", sipPass);
  p.putString("dname", displayName);
  p.putBool("sreg", sipRegister);
  p.putUChar("vol", volume);
  p.putUChar("mic", micGain);
  p.end();
}

void Settings::clearWifi() {
  wifiSsid = "";
  wifiPass = "";
  save();
}
