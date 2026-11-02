#pragma once

#include <Arduino.h>

// 設定値。NVS に保存する。
// Wi-Fi・音声・音量は stackchan-llm-mqtt-fft と同じ namespace "stackchan" を読むので、
// そのファームから書き換えた本体では設定し直さなくてよい。
struct Settings {
  // --- namespace "stackchan" ---
  String wifiSsid;
  String wifiPassword;
  String ttsEngine = "voicevox_compatible";  // builtin / voicevox_compatible / simple_wav
  String ttsHost = "192.168.1.2";
  uint16_t ttsPort = 50021;
  String ttsSpeaker = "3";
  uint8_t volume = 255;
  String timezone = "JST-9";

  // --- namespace "muse" ---
  String apiKey;  // Model API キー（ログや画面に出さない）
  String model = "muse-spark-1.3";
  String reasoning = "minimal";  // 会話は待たせないよう最短にする
  bool autoListen = true;        // 話し終えたら続けて聞く

  void load();
  void saveWifi(const String& ssid, const String& password);
  void saveApiKey(const String& key);
  void saveVolume(uint8_t value);
  void saveModel(const String& value);
  void saveAutoListen(bool value);
};
