#pragma once

#include <Arduino.h>

class AvatarFaceController;

// キャリブレーション結果を保持する構造体。NVSに永続保存される。
struct CalibrationData {
  // サーボキャリブレーション結果
  bool servoValid = false; // サーボキャリブレーションが完了しているかどうか
  int yawMin = 0;          // ヨー（左右）の最小角度（0.1度単位）
  int yawMax = 0;          // ヨー（左右）の最大角度
  int pitchMin = 0;        // ピッチ（上下）の最小角度
  int pitchMax = 0;        // ピッチ（上下）の最大角度

  // IMU水平キャリブレーション結果
  bool imuLevelValid = false;   // IMUキャリブレーションが完了しているかどうか
  float levelRollDeg = 0.0f;   // 水平状態でのロール角（度）。LEVEL HOLDモードの基準値。
  float levelPitchDeg = 0.0f;  // 水平状態でのピッチ角（度）。LEVEL HOLDモードの基準値。
};

// サーボとIMUのキャリブレーションを実行・保存・読み込みするクラス。
//
// キャリブレーション手順（run()呼び出し時）:
//   1. サーボキャリブレーション（7ステップ）:
//      - 手動でホーム位置を合わせてSET HOME
//      - 自動で全端点（YAW左右・PITCH上）へ移動して到達角度を記録
//      - 記録した範囲をNVSに保存
//   2. IMU水平キャリブレーション:
//      - ロボットを水平面に置いて静止
//      - 8秒間のジャイロキャリブレーション
//      - 300サンプルの加速度平均からロール・ピッチゼロ点を算出
//      - NVSに保存
class CalibrationController {
 public:
  // NVSからキャリブレーションデータを読み込む（起動時に呼ぶ）
  void load();

  // キャリブレーションを対話的に実行する。
  // アバターの描画を一時停止してUIを表示し、完了後に再開する。
  // 戻り値: 全ステップ成功=true、キャンセルまたはエラー=false
  bool run(AvatarFaceController& avatarFace);

  // 現在のキャリブレーションデータへの参照を返す
  const CalibrationData& data() const;

 private:
  CalibrationData data_; // 読み込み・計測したキャリブレーションデータ

  // サーボの可動範囲キャリブレーションを実行する（7ステップUI）
  bool calibrateServos();

  // IMUの水平基準点キャリブレーションを実行する
  bool calibrateImuLevel();

  // 指定位置にサーボを動かし、実際に到達した角度を測定する。
  //   requireTarget=true: 目標位置に到達できなければエラー
  //   assumeTargetAfterStoppedDelay=true: 停止後10秒経過で目標値とみなす（ピッチ上限用）
  bool moveAndVerify(
      const char* title, int yaw, int pitch, int* measuredYaw,
      int* measuredPitch, bool requireTarget,
      bool assumeTargetAfterStoppedDelay = false);

  // サーボキャリブレーションデータをNVSに保存する
  void saveServoCalibration();

  // IMUキャリブレーションデータをNVSに保存する
  void saveImuCalibration();

  // サーボのトルクをオフにして電源を切る（安全停止）
  void stopServos();
};
