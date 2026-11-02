#include "CameraView.h"

#include <esp_camera.h>
#include <img_converters.h>

namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 240;
constexpr uint8_t kJpegQuality = 80;
constexpr int kStaleFrames = 2;   // 古いフレームを読み捨てる
constexpr int kWarmUpFrames = 12;  // 起動の直後は自動露出が落ち着くまで待つ

// M5CoreS3 ライブラリと同じ配線。読み出しは QVGA（画面と同じ大きさ）
camera_config_t cameraConfig = {
    .pin_pwdn = -1, .pin_reset = -1, .pin_xclk = -1,
    .pin_sscb_sda = 12, .pin_sscb_scl = 11,
    .pin_d7 = 47, .pin_d6 = 48, .pin_d5 = 16, .pin_d4 = 15,
    .pin_d3 = 42, .pin_d2 = 41, .pin_d1 = 40, .pin_d0 = 39,
    .pin_vsync = 46, .pin_href = 38, .pin_pclk = 45,
    .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_RGB565,
    .frame_size = FRAMESIZE_QVGA,
    .jpeg_quality = 0, .fb_count = 2,
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_LATEST,
    .sccb_i2c_port = -1,
};

}  // namespace

bool CameraView::begin() {
  if (ready_) return true;
  // cam_task のスタックは小さく、ログを出すだけであふれることがある
  esp_log_level_set("cam_hal", ESP_LOG_NONE);
  M5.In_I2C.release();  // 内部 I2C を解放してカメラの SCCB に渡す
  const esp_err_t err = esp_camera_init(&cameraConfig);
  if (err != ESP_OK) {
    Serial.printf("[camera] init failed: 0x%x\n", err);
    return false;
  }
  ready_ = true;
  Serial.println("[camera] ready");
  return true;
}

void CameraView::end() {
  if (!ready_) return;
  esp_camera_deinit();
  ready_ = false;
  // カメラが使っていた内部 I2C を取り戻す
  M5.In_I2C.begin(M5.In_I2C.getPort(), M5.In_I2C.getSDA(), M5.In_I2C.getSCL());
}

PhotoPtr CameraView::capture(bool warmUp) {
  if (!ready_) return nullptr;
  const uint32_t started = millis();
  const int discard = warmUp ? kWarmUpFrames : kStaleFrames;
  for (int i = 0; i < discard; ++i) {
    camera_fb_t* stale = esp_camera_fb_get();
    if (stale != nullptr) esp_camera_fb_return(stale);
  }
  camera_fb_t* frame = esp_camera_fb_get();
  if (frame == nullptr) {
    Serial.println("[camera] no frame");
    return nullptr;
  }
  auto photo = std::make_shared<Photo>();
  const bool ok = frame2jpg(frame, kJpegQuality, &photo->jpeg, &photo->bytes);
  esp_camera_fb_return(frame);
  captureMs_ = millis() - started;
  if (!ok) {
    Serial.println("[camera] jpeg failed");
    return nullptr;
  }
  Serial.printf("[camera] photo %u bytes in %ums\n", static_cast<unsigned>(photo->bytes),
                static_cast<unsigned>(captureMs_));
  return photo;
}

bool CameraView::openPreview(bool testPattern) {
  testPattern_ = testPattern;
  canvas_.setPsram(true);
  canvas_.setColorDepth(16);
  if (canvas_.createSprite(kWidth, kHeight) == nullptr) {
    Serial.println("[camera] no memory for preview");
    return false;
  }
  return true;
}

void CameraView::closePreview() {
  canvas_.deleteSprite();
}

void CameraView::drawPreview() {
  if (testPattern_) {
    static constexpr uint16_t kBars[] = {TFT_WHITE, TFT_YELLOW, TFT_CYAN, TFT_GREEN,
                                         TFT_MAGENTA, TFT_RED, TFT_BLUE, TFT_DARKGREY};
    constexpr int kCount = sizeof(kBars) / sizeof(kBars[0]);
    for (int i = 0; i < kCount; ++i) {
      canvas_.fillRect(i * kWidth / kCount, 0, kWidth / kCount + 1, kHeight, kBars[i]);
    }
  } else {
    camera_fb_t* frame = esp_camera_fb_get();
    if (frame == nullptr) return;
    // カメラの RGB565 は上位バイトが先に並ぶ
    canvas_.pushImage(0, 0, kWidth, kHeight, reinterpret_cast<const lgfx::swap565_t*>(frame->buf));
    esp_camera_fb_return(frame);
  }
  drawOverlay();
  canvas_.pushSprite(&M5.Display, 0, 0);
}

void CameraView::drawOverlay() {
  canvas_.setFont(&fonts::lgfxJapanGothicP_16);
  canvas_.setTextSize(1);

  // 閉じるボタン
  canvas_.fillCircle(26, 26, 18, TFT_BLACK);
  canvas_.drawCircle(26, 26, 18, TFT_WHITE);
  canvas_.drawLine(19, 19, 33, 33, TFT_WHITE);
  canvas_.drawLine(20, 19, 34, 33, TFT_WHITE);
  canvas_.drawLine(33, 19, 19, 33, TFT_WHITE);
  canvas_.drawLine(34, 19, 20, 33, TFT_WHITE);

  // シャッター
  canvas_.fillCircle(kWidth / 2, kHeight - 34, 26, TFT_WHITE);
  canvas_.drawCircle(kWidth / 2, kHeight - 34, 21, TFT_BLACK);
  canvas_.drawCircle(kWidth / 2, kHeight - 34, 20, TFT_BLACK);

  // 操作の案内
  constexpr char kHint[] = "タップで撮って話しかける";
  const int textWidth = canvas_.textWidth(kHint);
  canvas_.fillRoundRect((kWidth - textWidth) / 2 - 8, 12, textWidth + 16, 26, 8, TFT_BLACK);
  canvas_.setTextColor(TFT_WHITE, TFT_BLACK);
  canvas_.setTextDatum(middle_center);
  canvas_.drawString(kHint, kWidth / 2, 26);
}
