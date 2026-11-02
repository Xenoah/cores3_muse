#include "Body.h"

#include <M5StackChan.h>
#include <math.h>

#include "CalibrationController.h"

namespace {

// 角度は 0.1 度単位、速度は 0.1 度/秒単位
constexpr uint32_t kUpdateIntervalMs = 80;
constexpr int kIdleSpeed = 160;        // 16.0度/秒
// 大音量の発話と重なると USB 給電では電流が足りず USB が切れることがあった（72度/秒で確認）
constexpr int kGestureSpeed = 300;     // 30.0度/秒
constexpr int kHomeYaw = 0;            // 正面
constexpr int kHomePitch = 200;        // 20.0度上向き
constexpr int kMaxStep = 24;           // 1更新あたり2.4度

constexpr uint32_t kNodMs = 800;
constexpr uint32_t kJoyMs = 2400;
constexpr uint32_t kShakeMs = 1600;

int moveToward(int current, int desired, int maxStep) {
  if (desired > current) return min(current + maxStep, desired);
  if (desired < current) return max(current - maxStep, desired);
  return current;
}

}  // namespace

int Body::clampYaw(int value) const {
  return constrain(value, calibration_->yawMin, calibration_->yawMax);
}

int Body::clampPitch(int value) const {
  return constrain(value, calibration_->pitchMin, calibration_->pitchMax);
}

void Body::begin(const CalibrationData& calibration) {
  calibration_ = &calibration;
  if (!calibration.servoValid || running_) {
    return;
  }

  M5StackChan.setServoPowerEnabled(true);
  delay(80);
  M5StackChan.Motion.setAutoAngleSyncEnabled(true);
  M5StackChan.Motion.setAutoTorqueReleaseEnabled(false);
  M5StackChan.Motion.setTorqueEnabled(true);
  delay(40);

  const auto angles = M5StackChan.Motion.getCurrentAngles();
  yawTarget_ = clampYaw(angles.x);
  pitchTarget_ = clampPitch(angles.y);
  M5StackChan.Motion.move(yawTarget_, pitchTarget_, kIdleSpeed);
  // Synchronize once on startup; continuous targets keep the spring's current
  // position/velocity. Resyncing every 80ms amplified transient UART failures.
  M5StackChan.Motion.setAutoAngleSyncEnabled(false);
  poseStartedAt_ = millis();
  lastUpdateAt_ = 0;
  running_ = true;
  Serial.printf("[body] started current=(%d,%d)\n", yawTarget_, pitchTarget_);
}

void Body::release() {
  if (!running_) return;
  M5StackChan.Motion.stop();
  delay(30);
  M5StackChan.Motion.setTorqueEnabled(false);
  M5StackChan.Motion.setAutoAngleSyncEnabled(true);
  M5StackChan.Motion.setAutoTorqueReleaseEnabled(true);
  M5StackChan.setServoPowerEnabled(false);
  running_ = false;
}

void Body::setPose(Pose pose) {
  if (pose_ == pose) return;
  pose_ = pose;
  poseStartedAt_ = millis();
  if (pose == Pose::Listen) gesture_ = Gesture::None;
}

void Body::startGesture(Gesture gesture) {
  if (!running_) return;
  gesture_ = gesture;
  gestureStartedAt_ = millis();
}

void Body::nod() { startGesture(Gesture::Nod); }
void Body::joy() { startGesture(Gesture::Joy); }
void Body::shake() { startGesture(Gesture::Shake); }

void Body::moveTo(int yaw, int pitch, int speed, int maxStep) {
  yaw = moveToward(yawTarget_, clampYaw(yaw), maxStep);
  pitch = moveToward(pitchTarget_, clampPitch(pitch), maxStep);
  // 目標が変わらなければ命令しない（聞いている間はモーターを鳴らさない）
  if (yaw == yawTarget_ && pitch == pitchTarget_) return;
  yawTarget_ = yaw;
  pitchTarget_ = pitch;
  M5StackChan.Motion.move(yawTarget_, pitchTarget_, speed);
}

void Body::update() {
  if (!running_) return;
  const uint32_t now = millis();
  if (now - lastUpdateAt_ < kUpdateIntervalMs) return;
  lastUpdateAt_ = now;

  if (gesture_ != Gesture::None) {
    const uint32_t elapsed = now - gestureStartedAt_;
    const uint32_t duration = gesture_ == Gesture::Nod   ? kNodMs
                              : gesture_ == Gesture::Joy ? kJoyMs
                                                         : kShakeMs;
    if (elapsed < duration) {
      const float t = elapsed / static_cast<float>(duration);
      const float envelope = sinf(PI * t);
      int yaw = kHomeYaw;
      int pitch = kHomePitch;
      if (gesture_ == Gesture::Nod) {
        pitch -= static_cast<int>(roundf(60.0f * envelope));
      } else if (gesture_ == Gesture::Joy) {
        yaw += static_cast<int>(roundf(100.0f * sinf(2.0f * PI * 2.0f * t) * envelope));
      } else {
        yaw += static_cast<int>(roundf(60.0f * sinf(2.0f * PI * 2.0f * t) * envelope));
      }
      moveTo(yaw, pitch, kGestureSpeed, 30);
      return;
    }
    gesture_ = Gesture::None;
    poseStartedAt_ = now;
  }

  const float elapsed = (now - poseStartedAt_) / 1000.0f;
  switch (pose_) {
    case Pose::Idle:
      moveTo(kHomeYaw + static_cast<int>(roundf(18.0f * sinf(elapsed * 2.1f))),
             kHomePitch + static_cast<int>(roundf(7.0f * sinf(elapsed * 1.45f + 1.2f))),
             kIdleSpeed, kMaxStep);
      break;
    case Pose::Listen:
      break;
    case Pose::Think:
      moveTo(kHomeYaw + static_cast<int>(roundf(90.0f * sinf(elapsed * 0.9f))),
             kHomePitch + 50, kIdleSpeed, kMaxStep);
      break;
    case Pose::Speak:
      moveTo(kHomeYaw + static_cast<int>(roundf(35.0f * sinf(elapsed * 1.7f))),
             kHomePitch + static_cast<int>(roundf(12.0f * sinf(elapsed * 2.9f))),
             kIdleSpeed, kMaxStep);
      break;
  }
}
