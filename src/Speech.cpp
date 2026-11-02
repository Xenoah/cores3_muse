#include "Speech.h"

#include <WiFi.h>

namespace {

constexpr uint32_t kServerRetryMs = 60000;   // 落ちていたサーバーを次に試すまで
constexpr int32_t kProbeTimeoutMs = 300;     // 届かないサーバーで会話を待たせない

}  // namespace

void Speech::begin(const Settings& settings, LipSyncCallback lipSync, ServiceCallback service) {
  settings_ = &settings;
  server_.setCallbacks(lipSync, service);
  builtin_.setCallbacks(lipSync, service);
  if (!builtin_.begin()) {
    Serial.printf("[voice] builtin voice unavailable: %s\n", builtin_.lastError().c_str());
  }
}

bool Speech::serverConfigured() const {
  return settings_ != nullptr && settings_->ttsEngine != "builtin" &&
         !settings_->ttsHost.isEmpty();
}

bool Speech::serverReachable() {
  if (!serverConfigured() || WiFi.status() != WL_CONNECTED) return false;
  if (serverDownAt_ != 0 && millis() - serverDownAt_ < kServerRetryMs) return false;
  WiFiClient probe;
  const bool ok = probe.connect(settings_->ttsHost.c_str(), settings_->ttsPort, kProbeTimeoutMs);
  probe.stop();
  if (!ok) serverDownAt_ = millis() | 1;
  return ok;
}

bool Speech::say(const String& text) {
  if (serverReachable()) {
    TtsConfig config;
    config.host = settings_->ttsHost;
    config.port = settings_->ttsPort;
    config.speaker = settings_->ttsSpeaker;
    config.engineType = settings_->ttsEngine == "simple_wav" ? TtsEngineType::SimpleWav
                                                             : TtsEngineType::VoiceVoxCompatible;
    if (server_.speak(config, text)) {
      lastEngine_ = "server";
      serverDownAt_ = 0;
      return true;
    }
    serverDownAt_ = millis() | 1;
    Serial.printf("[voice] server failed (%s), using builtin voice\n",
                  server_.lastError().c_str());
  }
  if (!builtin_.isReady()) {
    lastEngine_ = "none";
    return false;
  }
  lastEngine_ = "builtin";
  return builtin_.speak(text);
}

void Speech::abort() {
  builtin_.abort();
  server_.abort();
}
