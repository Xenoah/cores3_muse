#pragma once

#include <Arduino.h>

struct CalibrationData;

// 首のサーボ。stackchan-llm-mqtt-fft の動かし方（校正範囲へのクランプ、
// 1 更新あたりの移動量の制限、起動時に一度だけ角度を同期）を会話向けにまとめたもの。
// サーボ校正が済んでいない本体では何もしない。
class Body {
 public:
  enum class Pose : uint8_t {
    Idle,   // ゆっくり揺れる
    Listen, // 少し上を向いて止まる（モーター音をマイクに入れない）
    Think,  // ゆっくり左右を見る
    Speak,  // 小さく揺れる
  };

  void begin(const CalibrationData& calibration);
  void setPose(Pose pose);
  void nod();    // うなずく
  void joy();    // 喜んで左右に振る
  void shake();  // 小さく首を振る
  void update();  // メインループから毎回呼ぶ
  void release();  // トルクと電源を切る（校正の前など）

  bool isRunning() const { return running_; }

 private:
  enum class Gesture : uint8_t { None, Nod, Joy, Shake };

  int clampYaw(int value) const;
  int clampPitch(int value) const;
  void startGesture(Gesture gesture);
  void moveTo(int yaw, int pitch, int speed, int maxStep);

  const CalibrationData* calibration_ = nullptr;
  bool running_ = false;
  Pose pose_ = Pose::Idle;
  Gesture gesture_ = Gesture::None;
  uint32_t gestureStartedAt_ = 0;
  uint32_t poseStartedAt_ = 0;
  uint32_t lastUpdateAt_ = 0;
  int yawTarget_ = 0;
  int pitchTarget_ = 0;
};
