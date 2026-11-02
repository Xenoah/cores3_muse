#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5StackChan.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <time.h>

#include <atomic>

#include "AvatarFaceController.h"
#include "Body.h"
#include "CalibrationController.h"
#include "MuseClient.h"
#include "ScreenShot.h"
#include "Settings.h"
#include "Speech.h"
#include "Version.h"

namespace {

AvatarFaceController avatarFace;
CalibrationController calibrationController;
Settings settings;
Body body;
Speech speech;
MuseClient muse;

// --- 画面の状態 -----------------------------------------------------------

enum class Phase : uint8_t {
  Connecting,  // Wi-Fi・時刻・API の確認中
  NoWifi,      // Wi-Fi が未設定、またはつながらない
  NoKey,       // API キーが未設定
  BadKey,      // API キーが拒否された
  Offline,     // Muse に届かない
  Ready,       // 使える
};

struct PhaseStyle {
  const char* chip;
  bool pulse;
  uint16_t background;
  uint16_t foreground;
  uint8_t r, g, b;  // 本体 LED
};

constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kWhite = 0xFFFF;
constexpr uint16_t kAlertRed = 0x6000;
constexpr uint16_t kConnectSlate = 0x1926;

PhaseStyle styleOf(Phase phase) {
  switch (phase) {
    case Phase::Connecting: return {"接続中", true, kConnectSlate, kWhite, 0, 0, 24};
    case Phase::NoWifi:     return {"Wi-Fi 未設定", false, kAlertRed, kWhite, 40, 0, 0};
    case Phase::NoKey:      return {"APIキー未設定", false, kAlertRed, kWhite, 40, 16, 0};
    case Phase::BadKey:     return {"APIキー無効", false, kAlertRed, kWhite, 40, 0, 0};
    case Phase::Offline:    return {"Muse に届きません", false, kAlertRed, kWhite, 40, 0, 0};
    case Phase::Ready:      return {"準備OK", false, kBlack, kWhite, 0, 12, 0};
  }
  return {"", false, kBlack, kWhite, 0, 0, 0};
}

Phase phase = Phase::Connecting;
uint32_t lastHudAt = 0;

// モデル ID を画面用の名前にする（muse-spark-1.3 → Muse Spark 1.3）
String modelLabel(const String& model) {
  String label;
  bool capitalize = true;
  for (size_t i = 0; i < model.length(); ++i) {
    char c = model[i];
    if (c == '-') {
      label += ' ';
      capitalize = true;
      continue;
    }
    label += capitalize ? static_cast<char>(toupper(c)) : c;
    capitalize = false;
  }
  return label;
}

int wifiBars() {
  if (WiFi.status() != WL_CONNECTED) return 0;
  const int rssi = WiFi.RSSI();
  return rssi > -60 ? 3 : rssi > -72 ? 2 : 1;
}

void refreshHud() {
  const PhaseStyle style = styleOf(phase);
  HudData data;
  data.visible = true;
  strlcpy(data.chip, style.chip, sizeof(data.chip));
  data.chipPulse = style.pulse;
  strlcpy(data.label, modelLabel(settings.model).c_str(), sizeof(data.label));
  data.wifiBars = settings.wifiSsid.isEmpty() ? -1 : wifiBars();
  avatarFace.hud().set(data);
  lastHudAt = millis();
}

void setPhase(Phase next) {
  phase = next;
  const PhaseStyle style = styleOf(next);
  avatarFace.setColors(style.background, style.foreground);
  M5StackChan.showRgbColor(style.r, style.g, style.b);
  refreshHud();
}

// --- 口パク ---------------------------------------------------------------

std::atomic<int> lipSyncLevel{0};
float mouthRatio = 0.0f;

void setLipSyncLevel(int level) {
  lipSyncLevel = level;
}

void updateLipSync() {
  const float target = lipSyncLevel.load() / 100.0f;
  mouthRatio += (target - mouthRatio) * (target > mouthRatio ? 0.6f : 0.3f);
  avatarFace.setMouthOpenRatio(mouthRatio);
}

// --- ネットワーク ---------------------------------------------------------

constexpr uint32_t kWifiRetryMs = 20000;
constexpr uint32_t kOfflineRetryMs = 30000;
constexpr time_t kValidTimeAfter = 1700000000;  // これより前なら時刻は未取得

uint32_t wifiStartedAt = 0;
bool timeRequested = false;

// API の確認は TLS の接続で数秒かかるので専用タスクで行う
std::atomic<bool> apiCheckRunning{false};
std::atomic<bool> apiCheckDone{false};
MuseResult apiCheckResult;
int apiModelCount = 0;
uint32_t apiCheckAt = 0;
bool apiChecked = false;
int apiLastStatus = -1;

void startWifi() {
  if (settings.wifiSsid.isEmpty()) {
    setPhase(Phase::NoWifi);
    avatarFace.showCaption("USB から Wi-Fi を設定してね（tools/set_wifi.py）", 0);
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("stackchan-muse");
  WiFi.setSleep(false);  // 省電力より応答の速さを優先する
  WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPassword.c_str());
  wifiStartedAt = millis();
}

void apiCheckTask(void*) {
  muse.clearCancel();
  muse.setApiKey(settings.apiKey);
  apiCheckResult = muse.listModels(&apiModelCount);
  Serial.printf("[muse] check: status=%d ok=%d code=%s message=%s models=%d heap=%u\n",
                apiCheckResult.status, apiCheckResult.ok, apiCheckResult.code.c_str(),
                apiCheckResult.message.c_str(), apiModelCount,
                static_cast<unsigned>(ESP.getFreeHeap()));
  apiCheckDone = true;
  apiCheckRunning = false;
  vTaskDelete(nullptr);
}

void startApiCheck() {
  if (apiCheckRunning.exchange(true)) return;
  apiCheckDone = false;
  // TLS のハンドシェイクは深いスタックを使う
  xTaskCreatePinnedToCore(apiCheckTask, "apiCheck", 16384, nullptr, 1, nullptr, PRO_CPU_NUM);
}

void onApiChecked() {
  apiChecked = true;
  apiCheckAt = millis();
  apiLastStatus = apiCheckResult.status;
  if (apiCheckResult.ok) {
    setPhase(Phase::Ready);
    avatarFace.setExpression(m5avatar::Expression::Happy);
    avatarFace.showCaption("Muse につながりました", 4000);
    body.nod();
    speech.say("ミューズに、つながりました");
    avatarFace.setExpression(m5avatar::Expression::Neutral);
  } else if (apiCheckResult.status == 401 || apiCheckResult.status == 403) {
    if (settings.apiKey.isEmpty()) {
      setPhase(Phase::NoKey);
      avatarFace.setExpression(m5avatar::Expression::Doubt);
      avatarFace.showCaption("Muse まで届きました。USB から API キーを設定してね（tools/set_key.py）", 0);
    } else {
      setPhase(Phase::BadKey);
      avatarFace.setExpression(m5avatar::Expression::Sad);
      avatarFace.showCaption("API キーが使えないみたい。キーを確かめてね", 0);
    }
  } else {
    setPhase(Phase::Offline);
    avatarFace.setExpression(m5avatar::Expression::Sad);
    avatarFace.showCaption("Muse に届きません。少ししたらまた試します", 0);
  }
}

void updateNetwork() {
  const uint32_t now = millis();
  if (settings.wifiSsid.isEmpty()) return;

  if (WiFi.status() != WL_CONNECTED) {
    if (now - wifiStartedAt > kWifiRetryMs) {
      Serial.println("[wifi] retry");
      WiFi.disconnect();
      WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPassword.c_str());
      wifiStartedAt = now;
      if (phase != Phase::Connecting) setPhase(Phase::Connecting);
    }
    return;
  }

  // TLS の証明書の検証には正しい時刻が要る
  if (!timeRequested) {
    configTzTime(settings.timezone.c_str(), "ntp.nict.jp", "pool.ntp.org", "time.google.com");
    timeRequested = true;
    Serial.println("[wifi] connected, waiting for time");
  }
  if (time(nullptr) < kValidTimeAfter) return;

  if (apiCheckDone.exchange(false)) onApiChecked();
  if (!apiChecked) {
    startApiCheck();
  } else if (phase == Phase::Offline && now - apiCheckAt > kOfflineRetryMs) {
    startApiCheck();
    apiCheckAt = now;
  }
}

// --- USB シリアルのコマンド -----------------------------------------------

String consoleLine;

void printStatus() {
  JsonDocument doc;
  doc["version"] = kAppVersion;
  doc["phase"] = styleOf(phase).chip;
  doc["uptime_s"] = millis() / 1000;
  doc["reset_reason"] = static_cast<int>(esp_reset_reason());
  doc["wifi_set"] = !settings.wifiSsid.isEmpty();
  doc["wifi"] = WiFi.status() == WL_CONNECTED;
  doc["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  doc["time_ok"] = time(nullptr) >= kValidTimeAfter;
  doc["key_set"] = !settings.apiKey.isEmpty();
  doc["model"] = settings.model;
  doc["api_status"] = apiLastStatus;
  doc["api_code"] = apiCheckResult.code;
  doc["api_models"] = apiModelCount;
  doc["base_url_override"] = !muse.baseUrlOverride().isEmpty();
  doc["voice_clips"] = speech.clipCount();
  doc["voice_any_text"] = speech.canReadAnything();
  doc["voice_last_engine"] = speech.lastEngine();
  doc["volume"] = settings.volume;
  doc["servo_calibrated"] = calibrationController.data().servoValid;
  doc["body_running"] = body.isRunning();
  doc["heap"] = ESP.getFreeHeap();
  doc["heap_min"] = ESP.getMinFreeHeap();
  doc["psram"] = ESP.getFreePsram();
  doc["face_stack_free"] = avatarFace.drawStackFree();
  Serial.print("#STATUS ");
  serializeJson(doc, Serial);
  Serial.println();
}

void recheckApi() {
  muse.setApiKey(settings.apiKey);
  apiChecked = false;
  apiCheckDone = false;
  setPhase(Phase::Connecting);
  avatarFace.hud().clearCaption();
}

void runCalibration() {
  body.release();
  calibrationController.run(avatarFace);
  body.begin(calibrationController.data());
}

void handleCommand(const String& line) {
  const int space = line.indexOf(' ');
  const String command = space < 0 ? line : line.substring(0, space);
  String argument = space < 0 ? String() : line.substring(space + 1);
  argument.trim();

  if (command == "status") {
    printStatus();
  } else if (command == "shot") {
    dumpScreen(avatarFace);
  } else if (command == "colortest") {
    // スクリーンショットの色の確認用。赤・緑・青の順に左上へ描く
    const bool wasPaused = avatarFace.isDrawingPaused();
    if (!wasPaused) avatarFace.pauseDrawing();
    M5.Display.fillRect(0, 0, 40, 40, TFT_RED);
    M5.Display.fillRect(40, 0, 40, 40, TFT_GREEN);
    M5.Display.fillRect(80, 0, 40, 40, TFT_BLUE);
    M5.Display.waitDisplay();
    dumpScreen(avatarFace);
    if (!wasPaused) avatarFace.resumeDrawing();
  } else if (command == "say") {
    const bool ok = speech.say(argument);
    Serial.printf("#OK say=%d engine=%s\n", ok, speech.lastEngine());
  } else if (command == "setkey") {
    // キーそのものはログに出さない
    settings.saveApiKey(argument);
    recheckApi();
    Serial.println("#OK key saved");
  } else if (command == "wifi") {
    const int separator = argument.indexOf('\t');
    if (separator <= 0) {
      Serial.println("#ERR usage: wifi <ssid><TAB><password>");
      return;
    }
    settings.saveWifi(argument.substring(0, separator), argument.substring(separator + 1));
    Serial.println("#OK wifi saved, rebooting");
    Serial.flush();
    delay(100);
    ESP.restart();
  } else if (command == "volume") {
    settings.saveVolume(constrain(argument.toInt(), 0, 255));
    M5.Speaker.setVolume(settings.volume);
    Serial.println("#OK");
  } else if (command == "model") {
    settings.saveModel(argument);
    refreshHud();
    Serial.println("#OK");
  } else if (command == "baseurl") {
    // テスト用。保存しないので再起動で api.meta.ai に戻る
    muse.setBaseUrlOverride(argument);
    recheckApi();
    Serial.println("#OK");
  } else if (command == "check") {
    recheckApi();
    Serial.println("#OK");
  } else if (command == "nod") {
    body.nod();
    Serial.println("#OK");
  } else if (command == "joy") {
    body.joy();
    Serial.println("#OK");
  } else if (command == "shake") {
    body.shake();
    Serial.println("#OK");
  } else if (command == "calibrate") {
    runCalibration();
    Serial.println("#OK");
  } else if (command == "reboot") {
    Serial.println("#OK");
    Serial.flush();
    delay(100);
    ESP.restart();
  } else {
    Serial.printf("#ERR unknown command: %s\n", command.c_str());
  }
}

void updateConsole() {
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;
    if (c == '\n') {
      const String line = consoleLine;
      consoleLine = "";
      if (!line.isEmpty()) handleCommand(line);
      return;
    }
    if (consoleLine.length() < 1024) consoleLine += c;
  }
}

// --- メインループ ---------------------------------------------------------

// 音声の再生中にも画面・サーボを動かし続ける
void serviceApp() {
  M5StackChan.update();
  avatarFace.update();
  updateLipSync();
  body.update();
  if (millis() - lastHudAt > 2000) refreshHud();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.printf("\nStackChan x Muse %s, reset reason=%d\n", kAppVersion,
                static_cast<int>(esp_reset_reason()));

  M5StackChan.begin();
  // 不意に動かないよう、サーボは準備ができるまで切っておく
  M5StackChan.Motion.setTorqueEnabled(false);
  M5StackChan.setServoPowerEnabled(false);
  M5StackChan.showRgbColor(0, 0, 0);

  calibrationController.load();
  const auto& calibration = calibrationController.data();
  Serial.printf("Calibration servo=%d yaw=[%d,%d] pitch=[%d,%d]\n", calibration.servoValid,
                calibration.yawMin, calibration.yawMax, calibration.pitchMin,
                calibration.pitchMax);

  if (M5.Display.width() < M5.Display.height()) {
    M5.Display.setRotation(1);
  }
  M5.Display.setBrightness(160);
  M5.Display.fillScreen(TFT_BLACK);

  settings.load();

  avatarFace.begin();
  avatarFace.resetToDefault();
  avatarFace.setHudVisible(true);
  setPhase(Phase::Connecting);
  avatarFace.showCaption(String("StackChan x Muse ") + kAppVersion, 3000);

  M5.Speaker.begin();
  M5.Speaker.setVolume(settings.volume);
  speech.begin(settings, setLipSyncLevel, serviceApp);

  body.begin(calibration);
  startWifi();
}

void loop() {
  serviceApp();
  updateConsole();
  updateNetwork();
  delay(5);
}
