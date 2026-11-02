#pragma once

#include <Arduino.h>

#include "BuiltinVoice.h"
#include "Settings.h"
#include "VoiceVoxClient.h"

// 声。Muse には音声合成が無いので、声は本体側で作る。
// VOICEVOX サーバーが設定されていて届くならそれを使い、届かなければ内蔵ボイスで喋る。
class Speech {
 public:
  void begin(const Settings& settings, LipSyncCallback lipSync, ServiceCallback service);

  // 文章を読み上げる（ブロッキング、口パク付き）
  bool say(const String& text);
  void abort();

  bool isReady() const { return builtin_.isReady() || serverConfigured(); }
  uint16_t clipCount() const { return builtin_.isReady() ? builtin_.clipCount() : 0; }
  bool canReadAnything() const { return builtin_.canReadAnything(); }
  const char* lastEngine() const { return lastEngine_; }

 private:
  bool serverConfigured() const;
  bool serverReachable();

  const Settings* settings_ = nullptr;
  BuiltinVoice builtin_;
  TtsClient server_;
  uint32_t serverDownAt_ = 0;  // 0 = 問題なし
  const char* lastEngine_ = "none";
};
