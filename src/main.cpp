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
#include "CameraView.h"
#include "Conversation.h"
#include "MuseClient.h"
#include "Recorder.h"
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
Conversation conversation;
Recorder recorder;
CameraView cameraView;

// --- 見守り ---------------------------------------------------------------

// メインループが止まったら、どこで止まったかを残して再起動する。
// RTC_NOINIT は再起動しても消えないので、次の起動時に報告できる。
constexpr uint32_t kWedgedAfterMs = 30000;
constexpr uint32_t kWedgedMagic = 0x57454400;

RTC_NOINIT_ATTR uint32_t wedgedRecord;
uint32_t lastWedgedStage = 0;  // 前回止まった場所（0 = 止まっていない）
volatile uint32_t loopBeat = 0;
volatile uint8_t loopStage = 0;
volatile bool watchdogPaused = false;  // 校正中は長く待つことがある

void watchdogTask(void*) {
  uint32_t lastBeat = 0;
  uint32_t lastChange = millis();
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    const uint32_t now = millis();
    if (loopBeat != lastBeat || watchdogPaused) {
      lastBeat = loopBeat;
      lastChange = now;
    } else if (now - lastChange > kWedgedAfterMs) {
      wedgedRecord = kWedgedMagic | loopStage;
      Serial.printf("#WEDGED stage=%u\n", loopStage);
      Serial.flush();
      vTaskDelay(pdMS_TO_TICKS(100));
      esp_restart();
    }
  }
}

// --- 画面の状態 -----------------------------------------------------------

enum class Phase : uint8_t {
  Connecting,  // Wi-Fi・時刻・API の確認中
  NoWifi,      // Wi-Fi が未設定、またはつながらない
  NoKey,       // API キーが未設定
  BadKey,      // API キーが拒否された
  Offline,     // Muse に届かない
  Ready,       // 待機中
  Listening,   // 聞いている
  Transcribing,  // 聞き取った音声を文字にしている
  Thinking,    // Muse が考えている
  Speaking,    // 話している
  Preview,     // カメラのプレビュー
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
constexpr uint16_t kListenGreen = 0x0340;
constexpr uint16_t kThinkPurple = 0x3009;
constexpr uint16_t kSpeakNavy = 0x000F;

PhaseStyle styleOf(Phase phase) {
  switch (phase) {
    case Phase::Connecting: return {"接続中", true, kConnectSlate, kWhite, 0, 0, 24};
    case Phase::NoWifi:     return {"Wi-Fi 未設定", false, kAlertRed, kWhite, 40, 0, 0};
    case Phase::NoKey:      return {"APIキー未設定", false, kAlertRed, kWhite, 40, 16, 0};
    case Phase::BadKey:     return {"APIキー無効", false, kAlertRed, kWhite, 40, 0, 0};
    case Phase::Offline:    return {"Muse に届きません", false, kAlertRed, kWhite, 40, 0, 0};
    case Phase::Ready:      return {"待機中", false, kBlack, kWhite, 0, 8, 0};
    case Phase::Listening:  return {"聞いています", false, kListenGreen, kWhite, 0, 64, 0};
    case Phase::Transcribing: return {"聞き取り中", true, kThinkPurple, kWhite, 24, 0, 40};
    case Phase::Thinking:   return {"考え中", true, kThinkPurple, kWhite, 24, 0, 40};
    case Phase::Speaking:   return {"話しています", false, kSpeakNavy, kWhite, 0, 0, 48};
    case Phase::Preview:    return {"カメラ", false, kBlack, kWhite, 48, 48, 48};
  }
  return {"", false, kBlack, kWhite, 0, 0, 0};
}

Phase phase = Phase::Connecting;
uint32_t lastHudAt = 0;
bool cameraFailed = false;
PhotoPtr pendingPhoto;  // 次の質問に添える写真

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
  data.buttons = phase == Phase::Ready;
  data.cameraButton = !cameraFailed;
  data.photo = pendingPhoto != nullptr;
  data.meter = phase == Phase::Listening;
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

MuseResult apiCheckResult;
uint32_t apiCheckAt = 0;
bool apiChecked = false;
bool apiCheckRunning = false;
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

void startApiCheck() {
  if (apiCheckRunning) return;
  apiCheckRunning = true;
  conversation.checkApi();
}

void onApiChecked(const MuseResult& result) {
  apiCheckResult = result;
  apiCheckRunning = false;
  apiChecked = true;
  apiCheckAt = millis();
  apiLastStatus = apiCheckResult.status;
  if (apiCheckResult.ok) {
    setPhase(Phase::Ready);
    avatarFace.setExpression(m5avatar::Expression::Happy);
    avatarFace.showCaption("Muse につながりました。タップして話しかけてね", 5000);
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

  if (!apiChecked) {
    startApiCheck();
  } else if (phase == Phase::Offline && now - apiCheckAt > kOfflineRetryMs) {
    startApiCheck();
    apiCheckAt = now;
  }
}

// --- 会話 -----------------------------------------------------------------

constexpr uint32_t kFirstListenTimeoutMs = 7000;  // タップしたあと話し始めるまで
constexpr uint32_t kFollowUpTimeoutMs = 6000;     // 返事のあと、続けて話し始めるまで
// タップしたら小さな声も拾う。自動で聞くときは、テレビや物音で録音を始めないようにする
constexpr float kTapMinLevel = 110.0f;
constexpr float kFollowUpMinLevel = 220.0f;
constexpr char kPhotoQuestion[] = "この写真に何が写っているか教えて";

bool audioIsMic = false;
bool followUpTurn = false;
bool spokeInTurn = false;
bool moodShown = false;

m5avatar::Expression expressionOf(Mood mood) {
  switch (mood) {
    case Mood::Happy:  return m5avatar::Expression::Happy;
    case Mood::Sad:    return m5avatar::Expression::Sad;
    case Mood::Angry:  return m5avatar::Expression::Angry;
    case Mood::Doubt:  return m5avatar::Expression::Doubt;
    case Mood::Sleepy: return m5avatar::Expression::Sleepy;
    case Mood::Neutral: break;
  }
  return m5avatar::Expression::Neutral;
}

void useSpeaker() {
  if (!audioIsMic) return;
  M5.Mic.end();
  M5.Speaker.begin();
  M5.Speaker.setVolume(settings.volume);
  audioIsMic = false;
}

void useMic() {
  if (audioIsMic) return;
  M5.Speaker.stop();
  M5.Speaker.end();
  M5.Mic.begin();
  audioIsMic = true;
}

void backToReady() {
  pendingPhoto.reset();
  useSpeaker();
  setPhase(Phase::Ready);
  body.setPose(Body::Pose::Idle);
  avatarFace.setExpression(m5avatar::Expression::Neutral);
}

void startListening(bool followUp) {
  conversation.cancel();
  useSpeaker();
  followUpTurn = followUp;
  spokeInTurn = false;
  moodShown = false;
  avatarFace.hud().clearCaption();
  avatarFace.setExpression(m5avatar::Expression::Neutral);
  setPhase(Phase::Listening);
  body.setPose(Body::Pose::Listen);
  // マイクが開いたことを音で知らせる
  M5.Speaker.tone(followUp ? 1600 : 1200, 80);
  delay(110);
  useMic();
  recorder.start(followUp ? kFollowUpTimeoutMs : kFirstListenTimeoutMs,
                 followUp ? kFollowUpMinLevel : kTapMinLevel);
}

// 失敗を画面と声で伝える
void reportFailure(const MuseResult& result) {
  backToReady();
  String message;
  if (result.status == 401) {
    setPhase(Phase::BadKey);
    message = "API キーが使えないみたい。キーを確かめてね";
  } else if (result.status == 402 || result.status == 403) {
    message = "Muse に断られちゃった。クレジットや利用できる地域を確かめてね";
  } else if (result.status == 429) {
    message = "いま混み合っているみたい。少し待ってからまた話しかけてね";
  } else if (result.status == 0) {
    message = "ネットワークにつながらないみたい";
  } else {
    message = "うまくいかなかったよ。もう一度話しかけてね";
  }
  avatarFace.setExpression(m5avatar::Expression::Sad);
  avatarFace.showCaption(message, 0);
  body.shake();
  speech.say(message);
  avatarFace.hud().setCaption(message, 6000);
  if (phase == Phase::Ready) avatarFace.setExpression(m5avatar::Expression::Neutral);
}

void speakSentence(const String& text, Mood mood) {
  if (phase != Phase::Speaking) {
    setPhase(Phase::Speaking);
    body.setPose(Body::Pose::Speak);
  }
  avatarFace.setExpression(expressionOf(mood));
  avatarFace.showCaption(text, 0);
  if (!moodShown) {
    moodShown = true;
    if (mood == Mood::Happy) body.joy();
    else if (mood == Mood::Doubt) body.shake();
    else body.nod();
  }
  spokeInTurn = true;
  speech.say(text);
}

// 言葉から「見て」と頼まれたかどうかを判断する
bool wantsCamera(const String& text) {
  static const char* const kWords[] = {
      "見て", "見える", "見せ", "みて", "みえる", "みせ", "写真", "カメラ", "これ何", "これなに",
      "何これ", "なにこれ", "これは何", "これはなに", "look", "see this", "camera", "photo",
      "picture", "what is this"};
  String lower = text;
  lower.toLowerCase();
  for (const char* word : kWords) {
    if (lower.indexOf(word) >= 0) return true;
  }
  return false;
}

// 頼まれたらその場で撮る。LED を白くして撮っていることを知らせる
PhotoPtr lookNow() {
  if (cameraFailed) return nullptr;
  body.setPose(Body::Pose::Listen);  // 首を止めてぶれを防ぐ
  M5StackChan.showRgbColor(96, 96, 96);
  delay(150);
  PhotoPtr photo;
  if (cameraView.begin()) {
    photo = cameraView.capture(true);
    cameraView.end();
  } else {
    cameraFailed = true;
  }
  const PhaseStyle style = styleOf(phase);
  M5StackChan.showRgbColor(style.r, style.g, style.b);
  body.setPose(Body::Pose::Think);
  return photo;
}

void askMuse(const String& text, PhotoPtr photo) {
  setPhase(Phase::Thinking);
  body.setPose(Body::Pose::Think);
  avatarFace.showCaption(text, 0);
  conversation.submitText(text, std::move(photo));
}

void openPreview(bool testPattern) {
  if (!testPattern && !cameraView.begin()) {
    cameraFailed = true;
    refreshHud();
    avatarFace.showCaption("カメラが使えませんでした", 4000);
    return;
  }
  avatarFace.pauseDrawing();
  if (!cameraView.openPreview(testPattern)) {
    avatarFace.resumeDrawing();
    avatarFace.showCaption("メモリが足りません", 4000);
    return;
  }
  avatarFace.hud().clearCaption();
  setPhase(Phase::Preview);
  body.setPose(Body::Pose::Listen);
}

void closePreview() {
  cameraView.closePreview();
  cameraView.end();
  avatarFace.resumeDrawing();
}

void handlePreviewTap(int16_t x, int16_t y) {
  const bool closing = x >= 0 && x < kPreviewCloseSize && y >= 0 && y < kPreviewCloseSize;
  if (!closing) {
    pendingPhoto = cameraView.capture(false);
    if (pendingPhoto == nullptr) {
      closePreview();
      backToReady();
      avatarFace.showCaption("うまく撮れませんでした", 3000);
      return;
    }
  }
  closePreview();
  if (closing) {
    backToReady();
    return;
  }
  startListening(false);
}

void updateConversation() {
  if (phase == Phase::Listening) {
    switch (recorder.state()) {
      case Recorder::State::Recording:
        break;
      case Recorder::State::Finished:
        useSpeaker();
        setPhase(Phase::Transcribing);
        body.setPose(Body::Pose::Think);
        conversation.submitAudio(recorder.samples(), recorder.sampleCount(),
                                 Recorder::kSampleRate);
        break;
      case Recorder::State::NoSpeech:
        useSpeaker();
        if (pendingPhoto != nullptr) {
          askMuse(kPhotoQuestion, std::move(pendingPhoto));
          break;
        }
        backToReady();
        if (!followUpTurn) avatarFace.showCaption("聞こえなかったよ。もう一度タップしてね", 3000);
        break;
      default:
        backToReady();
        avatarFace.showCaption("マイクが使えませんでした", 4000);
        break;
    }
    return;
  }

  ConversationEvent* event = conversation.nextEvent();
  if (event == nullptr) return;
  switch (event->type) {
    case ConversationEvent::Type::ApiChecked:
      onApiChecked(event->result);
      break;
    case ConversationEvent::Type::Transcript:
      Serial.printf("[conv] user: %s\n", event->text.c_str());
      avatarFace.showCaption(event->text, 0);
      {
        PhotoPtr photo = std::move(pendingPhoto);
        if (photo == nullptr && wantsCamera(event->text)) {
          setPhase(Phase::Thinking);
          photo = lookNow();
        }
        askMuse(event->text, std::move(photo));
      }
      break;
    case ConversationEvent::Type::NoSpeech:
      if (pendingPhoto != nullptr) {
        askMuse(kPhotoQuestion, std::move(pendingPhoto));
        break;
      }
      backToReady();
      avatarFace.hud().clearCaption();
      if (!followUpTurn) avatarFace.showCaption("聞き取れなかったよ。もう一度タップしてね", 3000);
      break;
    case ConversationEvent::Type::Sentence:
      Serial.printf("[conv] muse: %s\n", event->text.c_str());
      speakSentence(event->text, event->mood);
      break;
    case ConversationEvent::Type::Done:
      if (spokeInTurn && settings.autoListen && phase == Phase::Speaking) {
        startListening(true);
      } else {
        backToReady();
        avatarFace.hud().clearCaption();
      }
      break;
    case ConversationEvent::Type::Failed:
      reportFailure(event->result);
      break;
  }
  delete event;
}

// --- タッチ ---------------------------------------------------------------

volatile bool tapPending = false;
int16_t tapX = 0;
int16_t tapY = 0;

// 話している途中でも受け付ける（serviceApp から呼ばれる）
void pollTouch() {
  const auto touch = M5.Touch.getDetail();
  bool tapped = false;
  if (touch.wasClicked()) {
    tapX = touch.x;
    tapY = touch.y;
    tapped = true;
  }
  if (M5StackChan.TouchSensor.wasSingleClicked()) {  // 頭をポンとたたいても同じ
    tapX = -1;
    tapY = -1;
    tapped = true;
  }
  if (!tapped) return;
  tapPending = true;
  if (phase == Phase::Speaking) speech.abort();
}

void handleTap() {
  if (!tapPending) return;
  tapPending = false;
  switch (phase) {
    case Phase::Ready:
      if (!cameraFailed && kHudCameraButton.contains(tapX, tapY)) {
        openPreview(false);
      } else {
        startListening(false);
      }
      break;
    case Phase::Preview:
      handlePreviewTap(tapX, tapY);
      break;
    case Phase::Listening:
      recorder.finish();  // 話し終わりをタップで伝える
      break;
    case Phase::Transcribing:
    case Phase::Thinking:
      conversation.cancel();
      backToReady();
      avatarFace.hud().clearCaption();
      break;
    case Phase::Speaking:
      startListening(false);  // 割り込んで話す
      break;
    default:
      break;
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
  doc["base_url_override"] = !muse.baseUrlOverride().isEmpty();
  doc["voice_clips"] = speech.clipCount();
  doc["voice_any_text"] = speech.canReadAnything();
  doc["voice_last_engine"] = speech.lastEngine();
  doc["volume"] = settings.volume;
  doc["servo_calibrated"] = calibrationController.data().servoValid;
  doc["body_running"] = body.isRunning();
  doc["auto_listen"] = settings.autoListen;
  doc["camera_ready"] = cameraView.isReady();
  doc["camera_failed"] = cameraFailed;
  doc["i2c_touch_ok"] = M5.In_I2C.scanID(0x38);
  doc["mic_noise"] = recorder.noiseFloor();
  doc["mic_peak"] = recorder.peakRms();
  doc["mic_gain"] = recorder.gain();
  doc["wedged_stage"] = lastWedgedStage;
  doc["heap"] = ESP.getFreeHeap();
  doc["heap_min"] = ESP.getMinFreeHeap();
  doc["psram"] = ESP.getFreePsram();
  doc["face_stack_free"] = avatarFace.drawStackFree();
  // 他のタスクのログと混ざらないよう、1 回の書き込みで出す
  String json;
  serializeJson(doc, json);
  Serial.print("#STATUS " + json + "\n");
}

void recheckApi() {
  muse.setApiKey(settings.apiKey);
  apiChecked = false;
  apiCheckRunning = false;
  conversation.cancel();
  setPhase(Phase::Connecting);
  avatarFace.hud().clearCaption();
}

void runCalibration() {
  watchdogPaused = true;
  body.release();
  calibrationController.run(avatarFace);
  body.begin(calibrationController.data());
  watchdogPaused = false;
}

// マイクの状態を測る（調整用）。音声は残さず、統計だけを出す
void printMicStats() {
  constexpr size_t kSamples = Recorder::kSampleRate;  // 1 秒
  auto* samples = static_cast<int16_t*>(
      heap_caps_malloc(kSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (samples == nullptr) {
    Serial.println("#ERR no memory");
    return;
  }
  useMic();
  // 起動直後の過渡音を読み捨ててから測る
  M5.Mic.record(samples, kSamples / 4, Recorder::kSampleRate);
  while (M5.Mic.isRecording()) delay(1);
  M5.Mic.record(samples, kSamples, Recorder::kSampleRate);
  while (M5.Mic.isRecording()) delay(1);
  useSpeaker();

  int64_t sum = 0;
  int64_t squares = 0;
  int peak = 0;
  uint32_t crossings = 0;
  for (size_t i = 0; i < kSamples; ++i) {
    sum += samples[i];
    squares += static_cast<int32_t>(samples[i]) * samples[i];
    peak = max(peak, abs(samples[i]));
    if (i > 0 && (samples[i] >= 0) != (samples[i - 1] >= 0)) ++crossings;
  }
  const float mean = static_cast<float>(sum) / kSamples;
  const float rms = sqrtf(max(0.0f, static_cast<float>(squares) / kSamples - mean * mean));
  Serial.printf("#MIC rms=%.0f mean=%.0f peak=%d crossings_per_s=%u first=", rms, mean, peak,
                static_cast<unsigned>(crossings));
  for (size_t i = 0; i < 12; ++i) Serial.printf("%d,", samples[i + 2000]);
  Serial.println();
  heap_caps_free(samples);
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
  } else if (command == "listen") {
    tapX = -1;
    tapY = -1;
    tapPending = true;
    Serial.println("#OK");
  } else if (command == "tap") {
    // 試験用に画面のタップを再現する: tap <x> <y>
    const int comma = argument.indexOf(' ');
    tapX = argument.substring(0, comma).toInt();
    tapY = argument.substring(comma + 1).toInt();
    tapPending = true;
    Serial.println("#OK");
  } else if (command == "preview") {
    // "preview test" は映像の代わりに模様を出す
    openPreview(argument == "test");
    Serial.println("#OK");
  } else if (command == "photo") {
    // 撮って大きさだけ報告する。写真そのものは出さない
    const PhotoPtr photo = cameraView.begin() ? cameraView.capture(true) : nullptr;
    cameraView.end();
    Serial.printf("#OK photo=%u ms=%u\n", photo ? static_cast<unsigned>(photo->bytes) : 0,
                  static_cast<unsigned>(cameraView.captureMs()));
  } else if (command == "ask") {
    // 聞き取ったことにして Muse に尋ねる（マイクを使わない試験用）
    if (phase != Phase::Ready && phase != Phase::Speaking) {
      Serial.println("#ERR busy");
      return;
    }
    conversation.cancel();
    spokeInTurn = false;
    moodShown = false;
    setPhase(Phase::Thinking);
    body.setPose(Body::Pose::Think);
    avatarFace.showCaption(argument, 0);
    conversation.submitText(argument, wantsCamera(argument) ? lookNow() : nullptr);
    Serial.println("#OK");
  } else if (command == "forget") {
    conversation.forget();
    Serial.println("#OK");
  } else if (command == "autolisten") {
    settings.saveAutoListen(argument != "0");
    Serial.println("#OK");
  } else if (command == "check") {
    recheckApi();
    Serial.println("#OK");
  } else if (command == "micstats") {
    printMicStats();
  } else if (command == "servo") {
    if (argument == "0") {
      body.release();
    } else {
      body.begin(calibrationController.data());
    }
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
  ++loopBeat;
  loopStage = 1;
  M5StackChan.update();
  loopStage = 2;
  pollTouch();
  loopStage = 3;
  avatarFace.update();
  loopStage = 4;
  updateLipSync();
  loopStage = 5;
  body.update();
  loopStage = 6;
  if (millis() - lastHudAt > 2000) refreshHud();
  loopStage = 7;
}

}  // namespace

void setup() {
  Serial.setTxBufferSize(4096);  // スクリーンショットの送信で溢れないように
  Serial.begin(115200);
  delay(100);
  Serial.printf("\nStackChan x Muse %s, reset reason=%d\n", kAppVersion,
                static_cast<int>(esp_reset_reason()));
  if ((wedgedRecord & 0xFFFFFF00) == kWedgedMagic) {
    lastWedgedStage = wedgedRecord & 0xFF;
    Serial.printf("[watchdog] the last run stopped at stage %u\n",
                  static_cast<unsigned>(lastWedgedStage));
  }
  wedgedRecord = 0;

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
  conversation.begin(muse, settings);
  if (!recorder.begin([](uint8_t level) { avatarFace.hud().pushMeter(level); })) {
    Serial.println("[mic] no memory for recording");
  }

  body.begin(calibration);
  startWifi();
  xTaskCreatePinnedToCore(watchdogTask, "watchdog", 3072, nullptr, 1, nullptr, PRO_CPU_NUM);
}

void loop() {
  serviceApp();
  loopStage = 10;
  updateConsole();
  loopStage = 11;
  updateNetwork();
  loopStage = 12;
  handleTap();
  loopStage = 13;
  updateConversation();
  if (phase == Phase::Preview) {
    loopStage = 15;
    cameraView.drawPreview();
  }
  loopStage = 14;
  delay(5);
}
