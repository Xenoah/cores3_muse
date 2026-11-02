#include "CalibrationController.h"

#include <M5Unified.h>
#include <Preferences.h>
#include <math.h>

#include "AvatarFaceController.h"
#include <M5StackChan.h>

namespace {

// サーボの物理的な可動範囲上限値（0.1度単位）
constexpr int kYawMin = -1280;   // ヨー最大左（-128.0度）
constexpr int kYawMax = 1280;    // ヨー最大右（+128.0度）
constexpr int kPitchMin = 0;     // ピッチ最小（0度 = ホーム位置）
constexpr int kPitchMax = 900;   // ピッチ最大上（90.0度）

// 自動移動時の速度（0.1度/秒単位）
constexpr int kSweepSpeed = 120;

// 到達判定の許容誤差（0.1度単位）
constexpr int kPositionTolerance = 80;  // ±8.0度

// 目標位置到達後、安定したとみなすまでの待機時間
constexpr uint32_t kMoveTimeoutMs = 30000;     // 移動タイムアウト（30秒）
constexpr uint32_t kPositionSettleMs = 300;    // 到達後の安定待機（300ms）
constexpr uint32_t kMinMoveWaitMs = 1200;      // 移動開始後の最小待機時間
constexpr uint32_t kPositionStableMs = 800;    // 位置変化なし=安定とみなす時間

// ピッチ上限でサーボが止まった場合に目標値とみなすまでの時間
constexpr uint32_t kAssumeTargetAfterStoppedMs = 10000;

// 「位置が安定している」と判断する変化量の閾値（0.1度単位）
constexpr int kStablePositionDelta = 10;

// 「実際に動いた」と判断する最小移動量（0.1度単位）
constexpr int kMinimumAcceptedTravel = 80;  // 8.0度

// IMUキャリブレーションの設定
constexpr uint32_t kImuCalibrationMs = 8000;  // ジャイロキャリブレーション時間（8秒）
constexpr size_t kImuSampleCount = 300;        // 加速度サンプル数

// キャリブレーション確認ダイアログのボタン
enum class PromptButton {
  None,
  Cancel,
  Confirm,
};

// ディスプレイ描画用キャンバス（フリッカーフリー描画のためダブルバッファ代替として使用）
M5Canvas& calibrationCanvas() {
  static M5Canvas canvas(&M5.Display);
  static int16_t canvasWidth = 0;
  static int16_t canvasHeight = 0;

  const int16_t width = M5.Display.width();
  const int16_t height = M5.Display.height();
  // 画面サイズが変わった場合（回転変更時など）はキャンバスを再作成する
  if (canvasWidth != width || canvasHeight != height) {
    canvas.deleteSprite();
    canvas.setColorDepth(16);
    canvas.createSprite(width, height);
    canvasWidth = width;
    canvasHeight = height;
  }
  return canvas;
}

// タッチ座標からキャリブレーションダイアログのボタンを判定する。
// ボタン領域: y=172〜230、左半分=Cancel、右半分=Confirm
PromptButton promptButtonAt(int16_t x, int16_t y) {
  if (y < 172 || y > 230) {
    return PromptButton::None;
  }
  return x < M5.Display.width() / 2
             ? PromptButton::Cancel
             : PromptButton::Confirm;
}

// キャリブレーション確認ダイアログを描画する。
//   title: 画面上部のタイトル（例: "SERVO 2/7"）
//   line1/line2: 説明テキスト（2行）
//   confirmLabel: 確認ボタンのラベル（例: "START", "SET HOME"）
//   pressed: 現在押されているボタン（押下ハイライト用）
//   footer: オプションの警告テキスト（黄色で表示）
void drawPrompt(
    const char* title, const char* line1, const char* line2,
    const char* confirmLabel, PromptButton pressed,
    const char* footer = nullptr) {
  auto& display = calibrationCanvas();
  const int16_t width = display.width();
  constexpr int16_t margin = 12;
  constexpr int16_t gap = 8;
  constexpr int16_t buttonY = 172;
  constexpr int16_t buttonH = 58;
  const int16_t buttonW = (width - margin * 2 - gap) / 2;
  const int16_t confirmX = margin + buttonW + gap;

  // 押下時はより明るい色で表示
  const uint16_t cancelColor =
      pressed == PromptButton::Cancel ? 0x7800 : 0x4208;  // 暗い赤 / 暗いグレー
  const uint16_t confirmColor =
      pressed == PromptButton::Confirm ? 0x03E0 : 0x0260; // 明るい緑 / 暗い緑

  display.fillScreen(TFT_BLACK);
  display.setFont(&fonts::Font2);
  display.setTextDatum(middle_center);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextSize(2);
  display.drawString(title, width / 2, 27);

  display.setTextSize(1);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.drawString(line1, width / 2, 73);
  display.drawString(line2, width / 2, 101);

  if (footer != nullptr) {
    display.setTextColor(TFT_YELLOW, TFT_BLACK); // 警告は黄色で強調
    display.drawString(footer, width / 2, 137);
  }

  display.fillRoundRect(
      margin, buttonY, buttonW, buttonH, 9, cancelColor);
  display.drawRoundRect(
      margin, buttonY, buttonW, buttonH, 9, TFT_WHITE);
  display.fillRoundRect(
      confirmX, buttonY, buttonW, buttonH, 9, confirmColor);
  display.drawRoundRect(
      confirmX, buttonY, buttonW, buttonH, 9, TFT_WHITE);

  display.setTextColor(TFT_WHITE);
  display.setTextSize(2);
  display.drawString("CANCEL", margin + buttonW / 2, buttonY + 29);
  display.drawString(
      confirmLabel, confirmX + buttonW / 2, buttonY + 29);
  display.pushSprite(0, 0);
}

// 画面から指が離れるまで待機する（タッチ開始状態でダイアログを開いた場合の誤操作防止）
void waitForTouchRelease() {
  int16_t x = 0;
  int16_t y = 0;
  while (M5.Display.getTouch(&x, &y)) {
    M5StackChan.update();
    delay(10);
  }
}

// 確認ダイアログを表示し、ユーザの選択を待つ。
// 戻り値: Confirm=true、Cancel=false
bool waitForConfirmation(
    const char* title, const char* line1, const char* line2,
    const char* confirmLabel, const char* footer = nullptr) {
  waitForTouchRelease();
  PromptButton pressed = PromptButton::None;
  bool wasTouching = false;
  drawPrompt(
      title, line1, line2, confirmLabel, PromptButton::None, footer);

  while (true) {
    M5StackChan.update();
    int16_t x = 0;
    int16_t y = 0;
    const bool touching = M5.Display.getTouch(&x, &y);
    const PromptButton current =
        touching ? promptButtonAt(x, y) : PromptButton::None;

    // ボタン変化時にダイアログを再描画（押下ハイライト）
    if (touching && current != pressed) {
      pressed = current;
      drawPrompt(title, line1, line2, confirmLabel, pressed, footer);
    }

    // 指が離れたとき、押していたボタンで判定する
    if (!touching && wasTouching) {
      const PromptButton selected = pressed;
      if (selected == PromptButton::Cancel) {
        return false;
      }
      if (selected == PromptButton::Confirm) {
        return true;
      }
      // ボタン外でリリースした場合はハイライト解除して待機継続
      pressed = PromptButton::None;
      drawPrompt(
          title, line1, line2, confirmLabel, pressed, footer);
    }

    wasTouching = touching;
    delay(10);
  }
}

// ホーム位置設定画面を表示し、ユーザが手動でサーボを位置合わせするのを待つ。
// サーボのトルクをOFFにして手で動かせる状態にし、
// 現在角度をリアルタイム（120ms更新）で表示する。
bool waitForHomeCapture() {
  waitForTouchRelease();
  PromptButton pressed = PromptButton::None;
  bool wasTouching = false;
  uint32_t lastDrawAt = 0;

  while (true) {
    M5StackChan.update();
    const uint32_t now = millis();

    int16_t x = 0;
    int16_t y = 0;
    const bool touching = M5.Display.getTouch(&x, &y);
    const PromptButton current =
        touching ? promptButtonAt(x, y) : PromptButton::None;

    if (touching && current != pressed) {
      pressed = current;
    }

    // 120msごとに現在角度を更新表示する
    if (now - lastDrawAt >= 120) {
      const auto angles = M5StackChan.Motion.getCurrentAngles();
      char position[64];
      snprintf(
          position, sizeof(position), "LIVE  Y:%+.1f  P:%+.1f",
          angles.x / 10.0f, angles.y / 10.0f);
      drawPrompt(
          "SERVO 1/7",
          "Move by hand: Yaw CENTER / Pitch HOME",
          position, "SET HOME", pressed,
          "Torque OFF - do not force the mechanism");
      lastDrawAt = now;
    }

    if (!touching && wasTouching) {
      if (pressed == PromptButton::Cancel) {
        return false;
      }
      if (pressed == PromptButton::Confirm) {
        return true; // SET HOMEが押された
      }
    }

    wasTouching = touching;
    delay(10);
  }
}

// サーボの自動移動中にリアルタイム状態を表示する画面を描画する
void drawMoving(
    const char* title, int targetYaw, int targetPitch,
    int currentYaw, int currentPitch) {
  auto& display = calibrationCanvas();
  char target[64];
  char current[64];
  snprintf(
      target, sizeof(target), "TARGET  Y:%+.1f  P:%+.1f",
      targetYaw / 10.0f, targetPitch / 10.0f);
  snprintf(
      current, sizeof(current), "NOW     Y:%+.1f  P:%+.1f",
      currentYaw / 10.0f, currentPitch / 10.0f);

  display.fillScreen(TFT_BLACK);
  display.setFont(&fonts::Font2);
  display.setTextDatum(middle_center);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextSize(2);
  display.drawString(title, display.width() / 2, 30);
  display.setTextSize(1);
  display.drawString(target, display.width() / 2, 80);
  display.drawString(current, display.width() / 2, 112);
  display.setTextColor(TFT_RED, TFT_BLACK);
  // 緊急停止の操作方法を赤で強調表示
  display.drawString(
      "TOUCH SCREEN TO EMERGENCY STOP",
      display.width() / 2, 185);
  display.pushSprite(0, 0);
}

// IMUキャリブレーション中の進捗画面を描画する
void drawImuProgress(
    uint32_t elapsedMs, float ax, float ay, float az) {
  auto& display = calibrationCanvas();
  char remaining[48];
  char accel[64];

  // 残り秒数（切り上げ）を計算する
  const uint32_t secondsLeft =
      (kImuCalibrationMs - min(elapsedMs, kImuCalibrationMs) + 999) /
      1000;
  snprintf(
      remaining, sizeof(remaining), "KEEP STILL: %lu sec",
      static_cast<unsigned long>(secondsLeft));
  snprintf(
      accel, sizeof(accel), "ACC  %.3f  %.3f  %.3f", ax, ay, az);

  display.fillScreen(TFT_BLACK);
  display.setFont(&fonts::Font2);
  display.setTextDatum(middle_center);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextSize(2);
  display.drawString("IMU LEVEL", display.width() / 2, 30);
  display.setTextSize(1);
  display.setTextColor(TFT_YELLOW, TFT_BLACK);
  display.drawString(
      "Place the whole robot on a level surface",
      display.width() / 2, 72);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.drawString(remaining, display.width() / 2, 118);
  display.drawString(accel, display.width() / 2, 148); // 加速度値をリアルタイム表示
  display.setTextColor(TFT_RED, TFT_BLACK);
  display.drawString(
      "TOUCH SCREEN TO CANCEL", display.width() / 2, 205);
  display.pushSprite(0, 0);
}

// タッチスクリーンが触れられているかどうかを確認する
bool screenTouched() {
  int16_t x = 0;
  int16_t y = 0;
  return M5.Display.getTouch(&x, &y);
}

// 現在位置が目標位置の許容範囲内（±8度）にあるかどうかを確認する
bool nearTarget(int yaw, int pitch, int targetYaw, int targetPitch) {
  return abs(yaw - targetYaw) <= kPositionTolerance &&
         abs(pitch - targetPitch) <= kPositionTolerance;
}

}  // namespace

// NVSのnamespace "stack_cal" からキャリブレーションデータを読み込む
void CalibrationController::load() {
  Preferences preferences;
  preferences.begin("stack_cal", true); // 読み取り専用で開く
  data_.servoValid = preferences.getBool("servo_valid", false);
  data_.yawMin = preferences.getInt("yaw_min", 0);
  data_.yawMax = preferences.getInt("yaw_max", 0);
  data_.pitchMin = preferences.getInt("pitch_min", 0);
  data_.pitchMax = preferences.getInt("pitch_max", 0);
  data_.imuLevelValid = preferences.getBool("imu_valid", false);
  data_.levelRollDeg = preferences.getFloat("roll_zero", 0.0f);
  data_.levelPitchDeg = preferences.getFloat("pitch_zero", 0.0f);
  preferences.end();
}

const CalibrationData& CalibrationController::data() const {
  return data_;
}

// キャリブレーションを実行するメインエントリ。
// アバター描画を停止してUIを表示し、サーボ→IMUの順に実行する。
bool CalibrationController::run(AvatarFaceController& avatarFace) {
  avatarFace.pauseDrawing(); // アバター描画タスクを停止
  delay(40);

  const bool servoOk = calibrateServos();
  bool imuOk = false;
  // サーボキャリブレーションが成功した場合のみIMUキャリブレーションを実行
  if (servoOk) {
    imuOk = calibrateImuLevel();
  }

  stopServos();               // 安全のためサーボを停止
  avatarFace.resumeDrawing(); // アバター描画を再開
  avatarFace.resetToDefault();

  if (servoOk && imuOk) {
    avatarFace.showStatus("CALIBRATION OK", 2500);
    M5StackChan.showRgbColor(0, 48, 0); // 成功: 緑色LED
    return true;
  }

  // キャンセルまたはエラー: 怒り表情と赤色LEDで通知
  avatarFace.setExpression(m5avatar::Expression::Angry);
  avatarFace.showStatus("CALIBRATION STOP", 3000);
  avatarFace.returnToDefaultAfter(3000);
  M5StackChan.showRgbColor(96, 0, 0); // エラー: 赤色LED
  return false;
}

// サーボの可動範囲キャリブレーション（7ステップ）。
//
// ステップ概要:
//   1/7: 手動でホーム位置に合わせてSET HOME
//   2/7: 自動計測開始の確認
//   3/7: YAW最小（左端）に移動して角度記録
//   4/7: YAW最大（右端）に移動して角度記録
//   5/7: センターに戻してPITCH最小角度を記録
//   6/7: PITCH最大（上）に移動して角度記録
//   7/7: ホームに戻す
bool CalibrationController::calibrateServos() {
  stopServos();
  M5StackChan.setServoPowerEnabled(true);
  delay(250); // サーボ電源安定待ち

  // トルクOFFにして手動でホーム位置を合わせる
  M5StackChan.Motion.setTorqueEnabled(false);

  if (!waitForHomeCapture()) {
    return false; // CANCELが押された
  }

  // 現在位置をホームとして登録する
  M5StackChan.Motion.setCurrentPostionAsHome();
  delay(150);
  const auto home = M5StackChan.Motion.getCurrentAngles();
  Serial.printf(
      "Servo home saved yaw=%d pitch=%d\n", home.x, home.y);

  // 自動計測開始の確認ダイアログ
  if (!waitForConfirmation(
          "SERVO 2/7",
          "Automatic full-range verification",
          "Runs all endpoints, then HOME",
          "START",
          "Clear cables and hands from the head")) {
    return false;
  }

  // トルクONにして自動移動モードを有効化する
  M5StackChan.Motion.setAutoAngleSyncEnabled(true);
  M5StackChan.Motion.setAutoTorqueReleaseEnabled(false);
  M5StackChan.Motion.move(home.x, home.y, 1000); // まずホーム位置へ
  delay(100);
  M5StackChan.Motion.setTorqueEnabled(true);
  delay(100);
  waitForTouchRelease(); // STARTボタンを離した後の誤反応を防ぐ

  int yawAtMin = 0;   // 記録するYAW最小到達角度
  int yawAtMax = 0;   // 記録するYAW最大到達角度
  int pitchAtMin = 0; // 記録するPITCH最小角度
  int pitchAtMax = 0; // 記録するPITCH最大角度
  int unused = 0;     // 不要な測定値の捨て先

  // 各端点に移動して到達角度を記録する
  // requireTarget=false: 物理限界で止まった位置を記録（目標値への到達は不要）
  if (!moveAndVerify(
          "SERVO 3/7 YAW LEFT", kYawMin, kPitchMin,
          &yawAtMin, &unused, false)) {
    return false;
  }
  if (!moveAndVerify(
          "SERVO 4/7 YAW RIGHT", kYawMax, kPitchMin,
          &yawAtMax, &unused, false)) {
    return false;
  }
  // requireTarget=true: センターへの復帰は正確に行う必要がある
  if (!moveAndVerify(
          "SERVO 5/7 CENTER", 0, kPitchMin,
          &unused, &pitchAtMin, true)) {
    return false;
  }
  // PITCH上限: 機種によっては止まりにくいため assumeTarget を使用
  if (!moveAndVerify(
          "SERVO 6/7 PITCH UP", 0, kPitchMax,
          &unused, &pitchAtMax, false, true)) {
    return false;
  }
  if (!moveAndVerify(
          "SERVO 7/7 HOME", 0, 0,
          &unused, &pitchAtMin, true)) {
    return false;
  }

  stopServos();

  // 測定結果をデータ構造に保存する
  data_.servoValid = true;
  data_.yawMin = yawAtMin;
  data_.yawMax = yawAtMax;
  data_.pitchMin = pitchAtMin;
  data_.pitchMax = pitchAtMax;
  saveServoCalibration();

  Serial.printf(
      "Servo calibration yaw=[%d,%d] pitch=[[%d,%d]\n",
      data_.yawMin, data_.yawMax,
      data_.pitchMin, data_.pitchMax);
  return true;
}

// サーボを指定位置に動かし、到達した実際の角度を測定・記録する。
//
// 終了条件:
//   A. 目標位置の±8度以内に入り、300ms安定 → 正常終了
//   B. requireTarget=false かつ 移動後に位置が安定 → 物理限界として記録
//   C. assumeTargetAfterStoppedDelay=true かつ 10秒経過して停止 → 目標値とみなす
//   D. タッチスクリーン操作 → 緊急停止（false返却）
//   E. 30秒タイムアウト → 停止（false返却）
bool CalibrationController::moveAndVerify(
    const char* title, int yaw, int pitch, int* measuredYaw,
    int* measuredPitch, bool requireTarget,
    bool assumeTargetAfterStoppedDelay) {
  M5StackChan.Motion.move(yaw, pitch, kSweepSpeed);
  const uint32_t startedAt = millis();
  uint32_t lastDrawAt = 0;
  uint32_t reachedSince = 0;  // 目標到達を最初に検出した時刻
  uint32_t stableSince = 0;   // 位置が安定し始めた時刻
  bool assumedTarget = false;
  const auto startAngles = M5StackChan.Motion.getCurrentAngles();
  auto lastStableAngles = startAngles;
  // 計画移動量（ヨーとピッチの大きい方）を基準に「十分動いたか」を判断する
  const int plannedTravel =
      max(abs(startAngles.x - yaw), abs(startAngles.y - pitch));

  while (true) {
    M5StackChan.update();
    const uint32_t now = millis();
    const uint32_t elapsed = now - startedAt;
    const auto current = M5StackChan.Motion.getCurrentAngles();

    // 120msごとにリアルタイム状態を画面更新する
    if (now - lastDrawAt >= 120) {
      drawMoving(title, yaw, pitch, current.x, current.y);
      lastDrawAt = now;
    }

    // 終了条件A: 目標近傍に入り300ms安定
    if (nearTarget(current.x, current.y, yaw, pitch)) {
      if (reachedSince == 0) {
        reachedSince = now;
      }
      if (now - reachedSince >= kPositionSettleMs) {
        break; // 到達確認
      }
    } else {
      reachedSince = 0; // 許容範囲から出たらリセット
    }

    // 安定判定: kStablePositionDelta(1度)以上動いた場合は安定タイマーをリセット
    if (abs(current.x - lastStableAngles.x) > kStablePositionDelta ||
        abs(current.y - lastStableAngles.y) > kStablePositionDelta) {
      lastStableAngles = current;
      stableSince = now;
    } else if (stableSince == 0) {
      stableSince = now;
    }

    // 実際の移動量（物理限界まで動いたかの判断）
    const int actualTravel =
        max(abs(current.x - startAngles.x),
            abs(current.y - startAngles.y));
    // 計画移動量が小さい（≤8度）か、実際に8度以上動いた場合を「十分動いた」とする
    const bool movedEnough =
        plannedTravel <= kMinimumAcceptedTravel ||
        actualTravel >= kMinimumAcceptedTravel;
    const bool stopped =
        !M5StackChan.Motion.isMoving() ||
        now - stableSince >= kPositionStableMs;

    // 終了条件B: requireTarget=false かつ 十分動いて安定（物理限界での記録）
    if (!requireTarget && !assumeTargetAfterStoppedDelay &&
        elapsed >= kMinMoveWaitMs &&
        movedEnough && now - stableSince >= kPositionStableMs) {
      Serial.printf(
          "%s settled before target target=(%d,%d) actual=(%d,%d)\n",
          title, yaw, pitch, current.x, current.y);
      break;
    }

    // 終了条件C: 10秒停止後に目標値とみなす（PITCH上限の物理限界対応）
    if (assumeTargetAfterStoppedDelay &&
        elapsed >= kAssumeTargetAfterStoppedMs &&
        movedEnough && stopped) {
      Serial.printf(
          "%s stopped for target assume target=(%d,%d) actual=(%d,%d)\n",
          title, yaw, pitch, current.x, current.y);
      assumedTarget = true;
      break;
    }

    // 終了条件D: タッチスクリーン操作で緊急停止（300ms後から有効）
    if (elapsed > 300 && screenTouched()) {
      Serial.printf("%s emergency stop\n", title);
      M5StackChan.Motion.stop();
      stopServos();
      waitForTouchRelease();
      return false;
    }

    // 終了条件E: タイムアウト
    if (elapsed >= kMoveTimeoutMs) {
      Serial.printf("%s timeout\n", title);
      M5StackChan.Motion.stop();
      stopServos();
      return false;
    }
    delay(20);
  }

  delay(150); // 最終位置を確定させるための短い待機
  const auto actual = M5StackChan.Motion.getCurrentAngles();
  // assumedTargetの場合は実測値ではなく目標値を使用する
  *measuredYaw = assumedTarget ? yaw : actual.x;
  *measuredPitch = assumedTarget ? pitch : actual.y;
  Serial.printf(
      "%s target=(%d,%d) actual=(%d,%d) measured=(%d,%d)\n",
      title, yaw, pitch, actual.x, actual.y,
      *measuredYaw, *measuredPitch);

  // requireTarget=trueなのに到達できなかった場合はエラーダイアログを表示
  if (requireTarget && !nearTarget(actual.x, actual.y, yaw, pitch)) {
    stopServos();
    waitForConfirmation(
        "SERVO ERROR",
        "Endpoint was not reached",
        "Check obstruction, wiring and servo",
        "CLOSE");
    return false;
  }
  return true;
}

// IMUの水平基準点キャリブレーション。
//
// 手順:
//   1. ロボットを水平面に置いて静止させる
//   2. 8秒間のジャイロキャリブレーション（内蔵センサのバイアス補正）
//   3. 300サンプルの加速度を収集して平均を計算
//   4. 重力ベクトルからRoll/Pitchのゼロ点を算出
//   5. 品質チェック（重力大きさ・ノイズ・ジャイロドリフト）
//   6. NVSに保存
bool CalibrationController::calibrateImuLevel() {
  if (!M5.Imu.isEnabled()) {
    Serial.println("IMU not found");
    return false;
  }

  if (!waitForConfirmation(
          "IMU LEVEL",
          "Place the whole robot on a level surface",
          "Do not touch it during calibration",
          "START",
          "Servos are powered OFF")) {
    return false;
  }

  waitForTouchRelease();
  // IMUのジャイロキャリブレーション開始（ジャイロのみ: 加速度・磁気は0でスキップ）
  M5.Imu.setCalibration(0, 64, 0);
  const uint32_t startedAt = millis();
  uint32_t lastDrawAt = 0;
  float ax = 0.0f;
  float ay = 0.0f;
  float az = 0.0f;

  // 8秒間のジャイロキャリブレーション（デバイスを静止させたままにする）
  while (millis() - startedAt < kImuCalibrationMs) {
    M5StackChan.update();
    M5.Imu.update();
    M5.Imu.getAccel(&ax, &ay, &az);

    const uint32_t elapsed = millis() - startedAt;
    if (elapsed - lastDrawAt >= 120) {
      drawImuProgress(elapsed, ax, ay, az);
      lastDrawAt = elapsed;
    }

    // タッチでキャンセル（300ms後から有効）
    if (elapsed > 300 && screenTouched()) {
      M5.Imu.setCalibration(0, 0, 0);   // キャリブレーション停止
      M5.Imu.loadOffsetFromNVS();        // 以前の補正値を復元
      waitForTouchRelease();
      return false;
    }
    delay(5);
  }
  M5.Imu.setCalibration(0, 0, 0); // キャリブレーション終了

  // 加速度データを300サンプル収集して統計処理する
  double accelSum[3] = {};       // 各軸の累積値（平均計算用）
  double accelSquareSum[3] = {}; // 各軸の二乗和（分散計算用）
  double gyroSum[3] = {};        // 各軸のジャイロ累積値（ドリフト確認用）
  size_t samples = 0;
  const uint32_t sampleStartedAt = millis();

  while (samples < kImuSampleCount &&
         millis() - sampleStartedAt < 10000) {
    M5StackChan.update();
    if (M5.Imu.update() == m5::IMU_Class::sensor_mask_none) {
      delay(2);
      continue; // センサ更新なしはスキップ
    }

    const auto& imu = M5.Imu.getImuData();
    for (size_t axis = 0; axis < 3; ++axis) {
      const double accel = imu.accel.value[axis];
      accelSum[axis] += accel;
      accelSquareSum[axis] += accel * accel;
      gyroSum[axis] += imu.gyro.value[axis];
    }
    ++samples;
    delay(5);
  }

  if (samples < kImuSampleCount) {
    M5.Imu.loadOffsetFromNVS();
    Serial.println("IMU sample timeout");
    return false;
  }

  // 平均・標準偏差・ジャイロ平均を計算する
  float accelAverage[3];
  float accelStdDev[3];
  float gyroAverage[3];
  for (size_t axis = 0; axis < 3; ++axis) {
    accelAverage[axis] = accelSum[axis] / samples;
    // 分散 = E[X²] - (E[X])²（二乗平均 - 平均の二乗）
    const double variance =
        max(0.0, accelSquareSum[axis] / samples -
                     accelAverage[axis] * accelAverage[axis]);
    accelStdDev[axis] = sqrt(variance);
    gyroAverage[axis] = gyroSum[axis] / samples;
  }

  // 品質チェック1: 重力の大きさが0.85〜1.15G（傾きすぎや外部振動がないことを確認）
  const float gravity = sqrtf(
      accelAverage[0] * accelAverage[0] +
      accelAverage[1] * accelAverage[1] +
      accelAverage[2] * accelAverage[2]);

  // 品質チェック2: 加速度ノイズが0.035G以下（振動がないことを確認）
  const float motionNoise =
      sqrtf(accelStdDev[0] * accelStdDev[0] +
            accelStdDev[1] * accelStdDev[1] +
            accelStdDev[2] * accelStdDev[2]);

  // 品質チェック3: ジャイロ角速度の合成が3.0deg/s以下（回転していないことを確認）
  const float gyroMotion =
      sqrtf(gyroAverage[0] * gyroAverage[0] +
            gyroAverage[1] * gyroAverage[1] +
            gyroAverage[2] * gyroAverage[2]);

  if (gravity < 0.85f || gravity > 1.15f ||
      motionNoise > 0.035f || gyroMotion > 3.0f) {
    M5.Imu.loadOffsetFromNVS();
    Serial.printf(
        "IMU unstable gravity=%.3f noise=%.4f gyro=%.3f\n",
        gravity, motionNoise, gyroMotion);
    waitForConfirmation(
        "IMU ERROR",
        "Robot moved or surface is not stable",
        "Place it level and retry calibration",
        "CLOSE");
    return false;
  }

  // 重力ベクトルからRoll/Pitchのゼロ点を算出する。
  // Roll: Y軸とZ軸の比率からロール角を計算
  // Pitch: X軸と(Y²+Z²)の合成から仰角を計算
  data_.levelRollDeg =
      atan2f(accelAverage[1], accelAverage[2]) * 180.0f / PI;
  data_.levelPitchDeg =
      atan2f(
          -accelAverage[0],
          sqrtf(accelAverage[1] * accelAverage[1] +
                accelAverage[2] * accelAverage[2])) *
      180.0f / PI;
  data_.imuLevelValid = true;

  M5.Imu.saveOffsetToNVS(); // ジャイロのバイアス補正値をNVSに保存
  saveImuCalibration();      // Roll/Pitchゼロ点をNVSに保存
  Serial.printf(
      "IMU level roll=%.3f pitch=%.3f gravity=%.3f\n",
      data_.levelRollDeg, data_.levelPitchDeg, gravity);
  return true;
}

// サーボキャリブレーションデータをNVSに保存する
void CalibrationController::saveServoCalibration() {
  Preferences preferences;
  preferences.begin("stack_cal", false); // 書き込みモードで開く
  preferences.putBool("servo_valid", data_.servoValid);
  preferences.putInt("yaw_min", data_.yawMin);
  preferences.putInt("yaw_max", data_.yawMax);
  preferences.putInt("pitch_min", data_.pitchMin);
  preferences.putInt("pitch_max", data_.pitchMax);
  preferences.end();
}

// IMUキャリブレーションデータをNVSに保存する
void CalibrationController::saveImuCalibration() {
  Preferences preferences;
  preferences.begin("stack_cal", false); // 書き込みモードで開く
  preferences.putBool("imu_valid", data_.imuLevelValid);
  preferences.putFloat("roll_zero", data_.levelRollDeg);
  preferences.putFloat("pitch_zero", data_.levelPitchDeg);
  preferences.end();
}

// サーボのトルクを無効化して電源を切る（安全停止）
void CalibrationController::stopServos() {
  M5StackChan.Motion.setTorqueEnabled(false);
  M5StackChan.Motion.setAutoTorqueReleaseEnabled(true);
  M5StackChan.setServoPowerEnabled(false);
}
