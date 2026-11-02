#include "Settings.h"

#include <Preferences.h>

namespace {

constexpr char kSharedNamespace[] = "stackchan";
constexpr char kMuseNamespace[] = "muse";

// 未保存のキーを getString すると Preferences がエラーログを出すので、存在を確かめてから読む
String readString(Preferences& prefs, const char* key, const String& fallback) {
  return prefs.isKey(key) ? prefs.getString(key, fallback) : fallback;
}

}  // namespace

void Settings::load() {
  Preferences prefs;
  if (prefs.begin(kSharedNamespace, true)) {
    wifiSsid = readString(prefs, "ssid", "");
    wifiPassword = readString(prefs, "password", "");
    ttsEngine = readString(prefs, "tts_engine", ttsEngine);
    ttsHost = readString(prefs, "tts_host", ttsHost);
    if (prefs.isKey("tts_port")) ttsPort = prefs.getUShort("tts_port", ttsPort);
    ttsSpeaker = readString(prefs, "tts_speaker", ttsSpeaker);
    if (prefs.isKey("volume")) volume = prefs.getUChar("volume", volume);
    timezone = readString(prefs, "tz", timezone);
    prefs.end();
  }
  // 読み取り専用だと namespace が無い初回に nvs_open のエラーログが出る
  if (prefs.begin(kMuseNamespace, false)) {
    apiKey = readString(prefs, "api_key", "");
    model = readString(prefs, "model", model);
    reasoning = readString(prefs, "reasoning", reasoning);
    if (prefs.isKey("auto_listen")) autoListen = prefs.getBool("auto_listen", autoListen);
    prefs.end();
  }
}

void Settings::saveWifi(const String& ssid, const String& password) {
  wifiSsid = ssid;
  wifiPassword = password;
  Preferences prefs;
  prefs.begin(kSharedNamespace, false);
  prefs.putString("ssid", ssid);
  prefs.putString("password", password);
  prefs.end();
}

void Settings::saveApiKey(const String& key) {
  apiKey = key;
  Preferences prefs;
  prefs.begin(kMuseNamespace, false);
  prefs.putString("api_key", key);
  prefs.end();
}

void Settings::saveVolume(uint8_t value) {
  volume = value;
  Preferences prefs;
  prefs.begin(kSharedNamespace, false);
  prefs.putUChar("volume", value);
  prefs.end();
}

void Settings::saveModel(const String& value) {
  model = value;
  Preferences prefs;
  prefs.begin(kMuseNamespace, false);
  prefs.putString("model", value);
  prefs.end();
}

void Settings::saveAutoListen(bool value) {
  autoListen = value;
  Preferences prefs;
  prefs.begin(kMuseNamespace, false);
  prefs.putBool("auto_listen", value);
  prefs.end();
}
