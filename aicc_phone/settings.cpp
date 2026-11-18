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
  uint8_t cfgv = p.getUChar("cfgv", 0);
  aecMode = p.getUChar("aec", DEF_AEC_MODE);
  micSlot = p.getUChar("mslot", DEF_MIC_SLOT);
  refSlot = p.getUChar("rslot", DEF_REF_SLOT);
  refDelayMs = p.getUShort("rdelay", DEF_REF_DELAY_MS);
  aecFilter = p.getUChar("afilt", DEF_AEC_FILTER);
  refGain = p.getUChar("rgain", DEF_REF_GAIN);
  micPga = p.getUChar("mpga", DEF_MIC_PGA);
  refDigGain = p.getUChar("rdig", DEF_REF_DIG_GAIN);
  resSuppress = p.getBool("rsup", DEF_RES_SUPPRESS);
  wakeSens = p.getUChar("wsens", DEF_WAKE_SENSITIVITY);
  aiKey = p.getString("aikey", "");
  aiHost = p.getString("aihost", DEF_AI_HOST);
  aiModel = p.getString("aimodel", DEF_AI_MODEL);
  aiVoice = p.getString("aivoice", DEF_AI_VOICE);
  aiExtra = p.getString("aiextra", DEF_AI_EXTRA);
  aiOutRate = p.getUInt("airate", DEF_AI_OUT_RATE);
  aiIdleSec = p.getUShort("aiidle", DEF_AI_IDLE_SEC);
  aiSearch = p.getBool("aisearch", DEF_AI_SEARCH);
  if (aiHost.length() == 0) aiHost = DEF_AI_HOST;
  if (aiModel.length() == 0) aiModel = DEF_AI_MODEL;
  if (aiOutRate < 8000 || aiOutRate > 48000) aiOutRate = DEF_AI_OUT_RATE;
  if (aiIdleSec < 20) aiIdleSec = DEF_AI_IDLE_SEC;
  if (wakeSens > 2) wakeSens = DEF_WAKE_SENSITIVITY;
  p.end();
  if (cfgv < AUDIO_CFG_VERSION) {
    // audio tuning defaults changed in this firmware: discard the stored ones
    aecMode = DEF_AEC_MODE; micSlot = DEF_MIC_SLOT; refSlot = DEF_REF_SLOT;
    refDelayMs = DEF_REF_DELAY_MS; aecFilter = DEF_AEC_FILTER; refGain = DEF_REF_GAIN;
    micPga = DEF_MIC_PGA; refDigGain = DEF_REF_DIG_GAIN; resSuppress = DEF_RES_SUPPRESS; micGain = DEF_MIC_GAIN;
    save();
  }
  if (aecMode > 3) aecMode = 1;
  if (refGain > 14) refGain = DEF_REF_GAIN;
  if (micPga > 14) micPga = DEF_MIC_PGA;
  if (refDigGain < 1 || refDigGain > 8) refDigGain = DEF_REF_DIG_GAIN;
  if (micSlot > 3) micSlot = DEF_MIC_SLOT;
  if (refSlot > 4) refSlot = DEF_REF_SLOT;
  if (refDelayMs > 400) refDelayMs = DEF_REF_DELAY_MS;
  if (aecFilter < 1 || aecFilter > 16) aecFilter = DEF_AEC_FILTER;
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
  p.putUChar("aec", aecMode);
  p.putUChar("mslot", micSlot);
  p.putUChar("rslot", refSlot);
  p.putUShort("rdelay", refDelayMs);
  p.putUChar("afilt", aecFilter);
  p.putUChar("rgain", refGain);
  p.putUChar("mpga", micPga);
  p.putUChar("rdig", refDigGain);
  p.putBool("rsup", resSuppress);
  p.putUChar("wsens", wakeSens);
  p.putString("aikey", aiKey);
  p.putString("aihost", aiHost);
  p.putString("aimodel", aiModel);
  p.putString("aivoice", aiVoice);
  p.putString("aiextra", aiExtra);
  p.putUInt("airate", aiOutRate);
  p.putUShort("aiidle", aiIdleSec);
  p.putBool("aisearch", aiSearch);
  p.putUChar("cfgv", AUDIO_CFG_VERSION);
  p.end();
}

void Settings::clearWifi() {
  wifiSsid = "";
  wifiPass = "";
  save();
}
