#pragma once

#include <Arduino.h>
#include <M5Unified.h>

#include <memory>

// 撮った写真（JPEG）。PSRAM に置く
struct Photo {
  uint8_t* jpeg = nullptr;
  size_t bytes = 0;
  ~Photo() { free(jpeg); }
};
using PhotoPtr = std::shared_ptr<Photo>;

// CoreS3 のカメラ（GC0308）。撮るときだけ起動し、終わったら end() で止める。
// 動かしたままだと発話や通信でフレームを取りこぼし、cam_task の警告ログでスタックが
// あふれて再起動することがあった。カメラとタッチは内部 I2C も共有している。
class CameraView {
 public:
  bool begin();
  void end();
  bool isReady() const { return ready_; }

  // 今の様子を撮る。ブロッキング。起動の直後は明るさが安定するまで warmUp を true にする
  PhotoPtr capture(bool warmUp);

  // 画面いっぱいのプレビュー。呼び出す側で顔の描画を止めておく。
  // testPattern は映像の代わりに模様を出す（公開用のスクリーンショットに部屋を写さない）
  bool openPreview(bool testPattern);
  void drawPreview();
  void closePreview();

  // 統計（調整用）
  uint32_t captureMs() const { return captureMs_; }

 private:
  void drawOverlay();

  bool ready_ = false;
  bool testPattern_ = false;
  uint32_t captureMs_ = 0;
  M5Canvas canvas_;
};

// プレビューの閉じるボタン
constexpr int16_t kPreviewCloseSize = 48;
