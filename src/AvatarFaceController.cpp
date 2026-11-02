#include "AvatarFaceController.h"

#include <M5Unified.h>
#include <esp_system.h>
#include <math.h>

namespace {

// 6種類の表情（インデックス順）
constexpr m5avatar::Expression kExpressions[] = {
    m5avatar::Expression::Happy,
    m5avatar::Expression::Angry,
    m5avatar::Expression::Sad,
    m5avatar::Expression::Doubt,
    m5avatar::Expression::Sleepy,
    m5avatar::Expression::Neutral,
};

// 表情の表示名
constexpr const char* kExpressionNames[] = {
    "HAPPY", "ANGRY", "SAD", "DOUBT", "SLEEPY", "NEUTRAL",
};

// 顔型の表示名
constexpr const char* kFaceNames[] = {
    "DEFAULT", "SIMPLE", "OMEGA", "GIRLY", "GIRLY 2", "PINK DEMON",
    "DOGGY",
};

// カラーパレットの表示名
constexpr const char* kPaletteNames[] = {
    "DEFAULT", "SKIN", "CYBER", "MONO", "DEMON",
};

// 目パターンの表示名
constexpr const char* kEyePatternNames[] = {
    "AUTO BLINK", "OPEN", "WINK LEFT", "WINK RIGHT", "CLOSED",
};

// 変形パターンの表示名
constexpr const char* kTransformNames[] = {
    "NORMAL", "ZOOM IN", "ZOOM OUT", "TILT LEFT", "TILT RIGHT",
};

// 表情・顔型切り替え後にデフォルトへ自動復帰するまでの時間（5秒）
constexpr uint32_t kDefaultReturnDelayMs = 5000;

// AutoBlinkモードのまばたき間隔: 3〜10秒のランダム
constexpr uint32_t kBlinkIntervalMinMs = 3000;
constexpr uint32_t kBlinkIntervalMaxMs = 10000;

// まばたき中に目を閉じている時間: 100〜180msのランダム
constexpr uint32_t kBlinkClosedMinMs = 100;
constexpr uint32_t kBlinkClosedMaxMs = 180;

// ゲーミングRGBの1周期（虹色を一巡する時間）: 6秒
constexpr uint32_t kGamingCyclePeriodMs = 6000;

// ゲーミングRGBのパレット更新間隔（throttle）: 約30fps
constexpr uint32_t kGamingUpdateIntervalMs = 33;

// HSV(0..1) を RGB565(16bit) に変換する。ゲーミング虹色パレット生成に使う。
uint16_t hsvToRgb565(float h, float s, float v) {
  h -= floorf(h); // 0..1 に正規化
  const float hf = h * 6.0f;
  const int i = static_cast<int>(hf) % 6;
  const float f = hf - floorf(hf);
  const float p = v * (1.0f - s);
  const float q = v * (1.0f - f * s);
  const float t = v * (1.0f - (1.0f - f) * s);
  float rf = 0, gf = 0, bf = 0;
  switch (i) {
    case 0: rf = v; gf = t; bf = p; break;
    case 1: rf = q; gf = v; bf = p; break;
    case 2: rf = p; gf = v; bf = t; break;
    case 3: rf = p; gf = q; bf = v; break;
    case 4: rf = t; gf = p; bf = v; break;
    default: rf = v; gf = p; bf = q; break;
  }
  const uint8_t r = static_cast<uint8_t>(rf * 255.0f);
  const uint8_t g = static_cast<uint8_t>(gf * 255.0f);
  const uint8_t b = static_cast<uint8_t>(bf * 255.0f);
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) |
                               (b >> 3));
}

// インデックスを循環させるヘルパー（負の方向にも対応）
size_t wrapIndex(size_t current, int direction, size_t count) {
  const int next = static_cast<int>(current) + direction;
  return static_cast<size_t>((next % static_cast<int>(count) +
                              static_cast<int>(count)) %
                             static_cast<int>(count));
}

}  // namespace

namespace m5avatar {
// m5stack-avatar の描画タスク（Avatar.cpp のグローバル）
extern TaskHandle_t drawTaskHandle;
}  // namespace m5avatar

namespace {

// 顔の描画タスクのスタック。m5stack-avatar の drawLoop は 2KB しかなく、
// 顔の上に HUD（日本語の字幕・温度など）を描くとあふれて再起動していた
constexpr uint32_t kFaceDrawStackBytes = 8192;
SemaphoreHandle_t faceFrameMutex = nullptr;

// 20fpsで十分な顔アニメーション。描画時間を含めて周期を制限する。
void faceDrawTask(void* arg) {
  auto* avatar = static_cast<m5avatar::Avatar*>(arg);
  for (;;) {
    const uint32_t started = millis();
    xSemaphoreTake(faceFrameMutex, portMAX_DELAY);
    avatar->draw();
    xSemaphoreGive(faceFrameMutex);
    const uint32_t elapsed = millis() - started;
    vTaskDelay(pdMS_TO_TICKS(elapsed < 50 ? 50 - elapsed : 1));
  }
}


}  // namespace

// アバターを初期化する。
// 重要: avatar_.init()（描画タスク起動）はセッターより先に呼ぶ必要がある。
// 先にセッターを呼ぶと描画タスクのハンドルがnullのまま内部でsuspend()が呼ばれ、
// setupタスク自体がブロックされてしまう。
void AvatarFaceController::begin() {
  if (started_) {
    return; // 二重初期化を防ぐ
  }

  initializeFaces();
  initializePalettes();
  randomSeed(esp_random()); // ハードウェア乱数でまばたきタイミングをランダム化

  avatar_.setPosition(0, 0); // ディスプレイ左上を原点に配置
  // init() の引数は描画スプライトの colorDepth（優先度ではない点に注意）。
  // 重要: m5avatar の Face::draw() は毎フレーム 320x240 のスプライトを
  // createSprite/deleteSprite する。colorDepth=8 だと 1フレームあたり約77KBの
  // 連続メモリ確保が必要になり、WiFi/TTS/HTML 等の確保とぶつかってヒープが
  // 断片化すると確保に失敗し、しばらく動かすとクラッシュする原因になる。
  // colorDepth=1 にすると約9.6KB/フレームに減り、断片化に強くなる
  // （見た目は StackChan 本来の2トーン表示。パレットの前景/背景色は反映される）。
  if (!replaceDrawTask()) {
    Serial.println("[face] drawing task allocation failed");
    return;
  }
  started_ = true;

  // 初期状態を全パラメータに適用する
  applyFace();
  applyPalette();
  applyExpression();
  applyEyePattern();
  applyTransform();
  scheduleNextBlink(millis());
}

// 毎フレームの更新処理。メインループから毎回呼ぶ。
void AvatarFaceController::update() {
  if (!started_) {
    return;
  }

  const uint32_t now = millis();

  // ステータスの自動消去（durationMs指定の場合）
  if (statusClearAt_ != 0 &&
      static_cast<int32_t>(now - statusClearAt_) >= 0) {
    applySpeechText("");
    statusClearAt_ = 0;
  }

  // ショーケースモードの自動進行（1.8秒ごと）
  if (showcaseEnabled_ &&
      static_cast<int32_t>(now - nextShowcaseAt_) >= 0) {
    advanceShowcase();
    nextShowcaseAt_ = now + 1800;
  }

  // デフォルト復帰タイマー（ショーケース中は無効）
  if (!showcaseEnabled_ && defaultReturnAt_ != 0 &&
      static_cast<int32_t>(now - defaultReturnAt_) >= 0) {
    resetToDefault();
  }

  const bool captionVisible = hud_.hasCaption();
  if (captionVisible != captionVisible_) {
    captionVisible_ = captionVisible;
    applyTransform();  // 表示開始時は等倍、終了時は選択した変形に戻す
  }

  // 呼吸に合わせた軽いズーム（Normal変形時のみ）。
  // 顔全体がゆっくり拡大縮小して、より生き生きと大げさに見せる。
  // HUD 表示中は HUD ごと拡大縮小されて読みにくくなるため止める。
  if (!showcaseEnabled_ && !hud_.isVisible() && !captionVisible_ &&
      transformPatternIndex_ ==
          static_cast<size_t>(TransformPattern::Normal)) {
    const float pulse = 1.0f + 0.06f * sinf(now * 0.0026f); // ~2.4秒周期で±6%
    avatar_.setScale(pulse);
  }

  updateBlink(now);

  // ゲーミングRGB（虹色循環）。有効時は毎フレーム末尾でパレットを更新する。
  updateGamingPalette(now);
}

// 表情を指定した種類に直接設定する（インデックスを検索して適用）
void AvatarFaceController::setExpression(
    m5avatar::Expression expression) {
  for (size_t i = 0; i < kExpressionCount; ++i) {
    if (kExpressions[i] == expression) {
      expressionIndex_ = i;
      applyExpression();
      return;
    }
  }
}

// 次の表情に切り替え、5秒後にデフォルトに戻す
void AvatarFaceController::nextExpression() {
  expressionIndex_ = (expressionIndex_ + 1) % kExpressionCount;
  applyExpression();
  showStatus(expressionName());
  returnToDefaultAfter(kDefaultReturnDelayMs);
}

// 次の顔型に切り替え、5秒後にデフォルトに戻す
void AvatarFaceController::nextFace() {
  faceIndex_ = (faceIndex_ + 1) % kFaceCount;
  applyFace();
  showStatus(faceName());
  returnToDefaultAfter(kDefaultReturnDelayMs);
}

// カラーパレットを切り替える（direction=+1で次、-1で前）
void AvatarFaceController::nextPalette(int direction) {
  paletteIndex_ = wrapIndex(paletteIndex_, direction, kPaletteCount);
  applyPalette();
  showStatus(paletteName());
  returnToDefaultAfter(kDefaultReturnDelayMs);
}

// 次の目パターンに切り替える
void AvatarFaceController::nextEyePattern() {
  eyePatternIndex_ = (eyePatternIndex_ + 1) % kEyePatternCount;
  applyEyePattern();
  showStatus(eyePatternName());
  returnToDefaultAfter(kDefaultReturnDelayMs);
}

// 次の変形パターンに切り替える
void AvatarFaceController::nextTransform() {
  transformPatternIndex_ =
      (transformPatternIndex_ + 1) % kTransformPatternCount;
  applyTransform();
  showStatus(transformName());
  returnToDefaultAfter(kDefaultReturnDelayMs);
}

// 口の開き具合を設定する（0.0〜1.0、範囲外はクランプ）
void AvatarFaceController::setMouthOpenRatio(float ratio) {
  avatar_.setMouthOpenRatio(constrain(ratio, 0.0f, 1.0f));
}

// 両目の視線方向を設定する（vertical: -1=上, +1=下 / horizontal: -1=左, +1=右）
void AvatarFaceController::setGaze(float vertical, float horizontal) {
  vertical = constrain(vertical, -1.0f, 1.0f);
  horizontal = constrain(horizontal, -1.0f, 1.0f);
  avatar_.setRightGaze(vertical, horizontal);
  avatar_.setLeftGaze(vertical, horizontal);
}

// ゲーミングRGBのON/OFFを切り替える。
// OFFにしたときは現在選択中のパレットを再適用して通常表示へ戻す。
void AvatarFaceController::setGamingRgb(bool enabled) {
  if (gamingRgb_ == enabled) {
    return;
  }
  gamingRgb_ = enabled;
  if (!gamingRgb_) {
    applyPalette(); // 通常パレットへ復帰
  } else {
    lastGamingUpdateAt_ = 0; // 次のupdate()で即座に虹色を適用
  }
}

bool AvatarFaceController::isGamingRgb() const {
  return gamingRgb_;
}

float AvatarFaceController::gamingHue() const {
  return gamingHue_;
}

// ゲーミングRGB有効時、時間に応じた虹色を顔のパレットに適用する。
// colorDepth=1（2トーン）描画のため、背景色と前景色を補色関係で循環させて
// コントラストを保ちつつ派手に色を変化させる。負荷軽減のためthrottleする。
void AvatarFaceController::updateGamingPalette(uint32_t now) {
  if (!gamingRgb_) {
    return;
  }
  if (lastGamingUpdateAt_ != 0 &&
      now - lastGamingUpdateAt_ < kGamingUpdateIntervalMs) {
    return;
  }
  lastGamingUpdateAt_ = now;

  gamingHue_ =
      static_cast<float>(now % kGamingCyclePeriodMs) / kGamingCyclePeriodMs;

  // 背景: 濃いめの虹色 / 前景(目・口): 補色の明るい虹色
  const uint16_t background = hsvToRgb565(gamingHue_, 1.0f, 0.6f);
  const uint16_t primary = hsvToRgb565(gamingHue_ + 0.5f, 1.0f, 1.0f);
  gamingPalette_.set(COLOR_BACKGROUND, background);
  gamingPalette_.set(COLOR_PRIMARY, primary);
  gamingPalette_.set(COLOR_SECONDARY, primary);
  avatar_.setColorPalette(gamingPalette_);
}

void AvatarFaceController::setColors(uint16_t background, uint16_t foreground) {
  gamingRgb_ = false;
  statePalette_.set(COLOR_BACKGROUND, background);
  statePalette_.set(COLOR_PRIMARY, foreground);
  statePalette_.set(COLOR_SECONDARY, foreground);
  avatar_.setColorPalette(statePalette_);
}

// ステータステキストを表示する。
// durationMs=0 の場合は明示的に消去するまで表示し続ける。
void AvatarFaceController::showStatus(const char* text,
                                      uint32_t durationMs) {
  if (hud_.isVisible()) {
    // 吹き出しは HUD 下段と重なるので、HUD のトースト表示へ回す
    applySpeechText("");
    hud_.setToast(text, durationMs);
    statusClearAt_ = 0;
    return;
  }
  applySpeechText(text);
  statusClearAt_ = durationMs == 0 ? 0 : millis() + durationMs;
}

void AvatarFaceController::showCaption(const String& text, uint32_t durationMs) {
  applySpeechText("");  // 標準吹き出しと字幕を重ねない
  statusClearAt_ = 0;
  hud_.setToast("", 0);
  hud_.setCaption(text, durationMs);
  captionVisible_ = hud_.hasCaption();
  applyTransform();
}

FaceHud& AvatarFaceController::hud() {
  return hud_;
}

void AvatarFaceController::setHudVisible(bool visible) {
  if (hud_.isVisible() == visible) {
    return;
  }
  HudData data;
  data.visible = visible;
  hud_.set(data);
  if (visible) {
    applySpeechText("");
    applyTransform();  // 呼吸ズームで変わったスケールを戻す
  }
}

// アバターのFreeRTOS描画タスクを一時停止する。
// 画面を直接描画する前（メニュー表示・キャリブレーション画面など）に呼ぶ。
void AvatarFaceController::pauseDrawing() {
  if (!started_ || drawingPaused_) {
    return;
  }
  drawingPaused_ = true;
  if (faceFrameMutex) {
    // Face::draw() yields while DMA still owns the LCD. A Blocked task state
    // does not mean a completed frame. Wait for the renderer to release it.
    xSemaphoreTake(faceFrameMutex, portMAX_DELAY);
  }
}

bool AvatarFaceController::replaceDrawTask() {
  faceFrameMutex = xSemaphoreCreateMutex();
  if (!faceFrameMutex) return false;
  // All avatar tasks use APP_CPU_NUM, as does Arduino's loop task. Keep the
  // library's 2KB draw task from running even once before replacing it.
  const UBaseType_t priority = uxTaskPriorityGet(nullptr);
  vTaskPrioritySet(nullptr, configMAX_PRIORITIES - 1);
  avatar_.init(1);
  TaskHandle_t library = m5avatar::drawTaskHandle;
  // 顔の描画はメインループ（優先度1）より後回し（優先度0）にする。メニュー・設定画面・
  // タッチの処理を先に済ませ、顔は空いた時間に描く。ハンドルは m5stack-avatar の
  // suspend()/resume()（setExpression が使う）からもこのタスクを指すよう差し替える
  TaskHandle_t task = nullptr;
  if (xTaskCreatePinnedToCore(faceDrawTask, "faceDraw", kFaceDrawStackBytes, &avatar_,
                              0, &task, APP_CPU_NUM) != pdPASS) {
    avatar_.stop();
    if (library) vTaskDelete(library);
    m5avatar::drawTaskHandle = nullptr;
    vSemaphoreDelete(faceFrameMutex);
    faceFrameMutex = nullptr;
    vTaskPrioritySet(nullptr, priority);
    return false;
  }
  m5avatar::drawTaskHandle = task;
  if (library) vTaskDelete(library);
  vTaskPrioritySet(nullptr, priority);
  return true;
}

uint32_t AvatarFaceController::drawStackFree() const {
  return m5avatar::drawTaskHandle ? uxTaskGetStackHighWaterMark(m5avatar::drawTaskHandle) : 0;
}

// アバターの描画タスクを再開する
void AvatarFaceController::resumeDrawing() {
  if (!started_ || !drawingPaused_) {
    return;
  }
  if (expressionPending_) {
    expressionPending_ = false;
    avatar_.setExpression(kExpressions[expressionIndex_]);
  }
  drawingPaused_ = false;
  if (faceFrameMutex) xSemaphoreGive(faceFrameMutex);
  else avatar_.resume();
}

// 全パラメータをデフォルト値にリセットする。
// expressionIndex_=5(Neutral), faceIndex_=0(Default), paletteIndex_=0(Default)
void AvatarFaceController::resetToDefault() {
  showcaseEnabled_ = false;
  expressionIndex_ = 5;  // Neutral
  faceIndex_ = 0;        // Default
  paletteIndex_ = 0;     // Default
  eyePatternIndex_ = 0;  // AutoBlink
  transformPatternIndex_ = 0; // Normal

  applySpeechText("");
  hud_.setToast("", 0);
  avatar_.setMouthOpenRatio(0.0f);
  applyFace();
  applyPalette();
  applyExpression();
  applyEyePattern();
  applyTransform();

  statusClearAt_ = 0;
  defaultReturnAt_ = 0;
}

// delayMsミリ秒後にresetToDefault()を呼ぶタイマーをセットする
void AvatarFaceController::returnToDefaultAfter(uint32_t delayMs) {
  defaultReturnAt_ = millis() + delayMs;
}

// ショーケースモードのON/OFFを切り替える。
// ONにすると1.8秒ごとに自動で表情・目・変形が変わる。
void AvatarFaceController::toggleShowcase() {
  showcaseEnabled_ = !showcaseEnabled_;
  defaultReturnAt_ = 0; // 自動復帰タイマーをキャンセル

  if (showcaseEnabled_) {
    nextShowcaseAt_ = millis(); // 即座に最初のサイクルを実行
    showStatus("SHOWCASE ON");
  } else {
    resetToDefault();
    showStatus("SHOWCASE OFF");
  }
}

bool AvatarFaceController::isShowcaseEnabled() const {
  return showcaseEnabled_;
}

const char* AvatarFaceController::expressionName() const {
  return kExpressionNames[expressionIndex_];
}

const char* AvatarFaceController::faceName() const {
  return kFaceNames[faceIndex_];
}

const char* AvatarFaceController::paletteName() const {
  return kPaletteNames[paletteIndex_];
}

const char* AvatarFaceController::eyePatternName() const {
  return kEyePatternNames[eyePatternIndex_];
}

const char* AvatarFaceController::transformName() const {
  return kTransformNames[transformPatternIndex_];
}

// 7種類の顔型オブジェクトを生成する。
// faces_[0]はAvatarのデフォルト顔（ライブラリ管理）、それ以外はnewで確保。
void AvatarFaceController::initializeFaces() {
  faces_[0] = avatar_.getFace();            // Default（ライブラリ所有）
  faces_[1] = new m5avatar::SimpleFace();   // シンプルな円形の目
  faces_[2] = new m5avatar::OmegaFace();    // 大きな目のΩ型
  faces_[3] = new m5avatar::GirlyFace();    // 女の子風
  faces_[4] = new m5avatar::GirlyFace2();   // 女の子風2（まつ毛付き）
  faces_[5] = new m5avatar::PinkDemonFace(); // ピンクの悪魔風
  faces_[6] = new m5avatar::DoggyFace();    // 犬風

  // 全顔テンプレートの口を HUD 付きの口で包む（口を描いた直後に HUD を重ねる）
  for (m5avatar::Face* face : faces_) {
    face->setMouth(new HudMouth(face->getMouth(), &hud_));
  }
}

// 5種類のカラーパレットを設定する。
// palettes_[0]はデフォルト（ライブラリのデフォルト色をそのまま使用）。
void AvatarFaceController::initializePalettes() {
  // Skin: 肌色背景、暗いグレーの顔パーツ、ピンクのアクセント
  palettes_[1].set(COLOR_PRIMARY, M5.Display.color24to16(0x383838));
  palettes_[1].set(COLOR_BACKGROUND, M5.Display.color24to16(0xFAC2A8));
  palettes_[1].set(COLOR_SECONDARY, TFT_PINK);

  // Cyber: シアン・黄色のサイバーパンク風
  palettes_[2].set(COLOR_PRIMARY, TFT_YELLOW);
  palettes_[2].set(COLOR_BACKGROUND, TFT_DARKCYAN);
  palettes_[2].set(COLOR_SECONDARY, TFT_CYAN);

  // Mono: モノクロ（白背景・グレーパーツ）
  palettes_[3].set(COLOR_PRIMARY, TFT_DARKGREY);
  palettes_[3].set(COLOR_BACKGROUND, TFT_WHITE);
  palettes_[3].set(COLOR_SECONDARY, TFT_LIGHTGREY);

  // Demon: 赤・ピンク・マゼンタの悪魔風
  palettes_[4].set(COLOR_PRIMARY, TFT_RED);
  palettes_[4].set(COLOR_BACKGROUND, TFT_PINK);
  palettes_[4].set(COLOR_SECONDARY, TFT_MAGENTA);
}

void AvatarFaceController::applyExpression() {
  if (!started_) return;
  // m5stack-avatar の setExpression() は描画タスクを止めて必ず再開する。
  // メニューなどで止めている間に呼ぶと顔が UI の上に描かれてしまうので、再開まで待つ
  if (drawingPaused_) {
    expressionPending_ = true;
    return;
  }
  if (faceFrameMutex) xSemaphoreTake(faceFrameMutex, portMAX_DELAY);
  avatar_.setExpression(kExpressions[expressionIndex_]);
  if (faceFrameMutex) xSemaphoreGive(faceFrameMutex);
}

void AvatarFaceController::applySpeechText(const char* text) {
  // pauseDrawing() 中は呼び出し側が既に同じ mutex を保持している。
  const bool lock = started_ && !drawingPaused_ && faceFrameMutex;
  if (lock) xSemaphoreTake(faceFrameMutex, portMAX_DELAY);
  avatar_.setSpeechText(text);
  if (lock) xSemaphoreGive(faceFrameMutex);
}

void AvatarFaceController::applyFace() {
  avatar_.setFace(faces_[faceIndex_]);
}

void AvatarFaceController::applyPalette() {
  avatar_.setColorPalette(palettes_[paletteIndex_]);
}

// 目パターンを適用する。
// AutoBlinkモードはライブラリ内蔵の自動まばたきを使わず、
// このクラスが独自にタイマー管理する（3〜10秒ランダム間隔）。
void AvatarFaceController::applyEyePattern() {
  const EyePattern pattern =
      static_cast<EyePattern>(eyePatternIndex_);

  switch (pattern) {
    case EyePattern::AutoBlink:
      // ライブラリのオートブリンクを無効にして、独自タイマーに切り替える
      avatar_.setIsAutoBlink(false);
      avatar_.setEyeOpenRatio(1.0f);
      blinkClosed_ = false;
      scheduleNextBlink(millis());
      break;
    case EyePattern::Open:
      avatar_.setIsAutoBlink(false);
      avatar_.setEyeOpenRatio(1.0f);
      blinkClosed_ = false;
      break;
    case EyePattern::WinkLeft:
      avatar_.setIsAutoBlink(false);
      avatar_.setLeftEyeOpenRatio(0.0f);   // 左目を閉じる
      avatar_.setRightEyeOpenRatio(1.0f);  // 右目は開けたまま
      blinkClosed_ = false;
      break;
    case EyePattern::WinkRight:
      avatar_.setIsAutoBlink(false);
      avatar_.setLeftEyeOpenRatio(1.0f);   // 左目は開けたまま
      avatar_.setRightEyeOpenRatio(0.0f);  // 右目を閉じる
      blinkClosed_ = false;
      break;
    case EyePattern::Closed:
      avatar_.setIsAutoBlink(false);
      avatar_.setEyeOpenRatio(0.0f);       // 両目を閉じる
      blinkClosed_ = true;
      break;
  }
}

// 変形パターンをアバターに適用する（スケール・回転を組み合わせる）
void AvatarFaceController::applyTransform() {
  const TransformPattern pattern =
      static_cast<TransformPattern>(transformPatternIndex_);

  // まずデフォルト値にリセット
  avatar_.setScale(1.0f);
  avatar_.setRotation(0.0f);

  if (hud_.hasCaption()) return;  // 字幕・FFTを等倍で表示

  switch (pattern) {
    case TransformPattern::Normal:
      break; // リセット済みのまま
    case TransformPattern::ZoomIn:
      avatar_.setScale(1.15f);  // 15%拡大
      break;
    case TransformPattern::ZoomOut:
      avatar_.setScale(0.85f);  // 15%縮小
      break;
    case TransformPattern::TiltLeft:
      avatar_.setRotation(-0.12f); // 左傾き（約7度）
      break;
    case TransformPattern::TiltRight:
      avatar_.setRotation(0.12f);  // 右傾き（約7度）
      break;
  }
}

// ショーケースモードで次のパターンに進める。
// 表情→目→変形の順にサイクルし、表情が一周したら顔型を変え、
// 顔型が一周したらパレットを変える。
void AvatarFaceController::advanceShowcase() {
  expressionIndex_ = (expressionIndex_ + 1) % kExpressionCount;
  eyePatternIndex_ = (eyePatternIndex_ + 1) % kEyePatternCount;
  transformPatternIndex_ =
      (transformPatternIndex_ + 1) % kTransformPatternCount;

  // 表情が一周したら次の顔型へ
  if (expressionIndex_ == 0) {
    faceIndex_ = (faceIndex_ + 1) % kFaceCount;
    applyFace();

    // 顔型も一周したら次のパレットへ
    if (faceIndex_ == 0) {
      paletteIndex_ = (paletteIndex_ + 1) % kPaletteCount;
      applyPalette();
    }
  }

  applyExpression();
  applyEyePattern();
  applyTransform();
  showStatus(expressionName(), 900); // 表情名を0.9秒間表示
}

// AutoBlinkモードの自動まばたきを更新する。
// タイムアウトの判定はuint32_tのラップアラウンドに対応するため
// int32_tにキャストしてから比較する。
void AvatarFaceController::updateBlink(uint32_t now) {
  // AutoBlinkモード以外では何もしない
  if (eyePatternIndex_ !=
      static_cast<size_t>(EyePattern::AutoBlink)) {
    return;
  }

  // 次のまばたき時刻になったら目を閉じる
  if (!blinkClosed_ &&
      static_cast<int32_t>(now - nextBlinkAt_) >= 0) {
    avatar_.setEyeOpenRatio(0.0f);
    blinkClosed_ = true;
    // 100〜180msのランダムな時間後に目を開ける
    blinkOpenAt_ =
        now + random(kBlinkClosedMinMs, kBlinkClosedMaxMs + 1);
    return;
  }

  // 目を閉じている時間が経過したら目を開けて次のまばたきをスケジュール
  if (blinkClosed_ &&
      static_cast<int32_t>(now - blinkOpenAt_) >= 0) {
    avatar_.setEyeOpenRatio(1.0f);
    blinkClosed_ = false;
    scheduleNextBlink(now);
  }
}

// 次のまばたき開始時刻をランダムにスケジュールする（3〜10秒後）
void AvatarFaceController::scheduleNextBlink(uint32_t now) {
  nextBlinkAt_ =
      now + random(kBlinkIntervalMinMs, kBlinkIntervalMaxMs + 1);
}
