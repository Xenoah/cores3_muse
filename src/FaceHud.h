#pragma once

#include <Arduino.h>
#include <Drawable.h>

// 顔の上に重ねる会話 UI の表示内容。
// メインループが set() で書き込み、アバター描画タスクが draw() で読む。
struct HudData {
  bool visible = false;
  char chip[28] = "";      // 左上の状態チップ（例: "聞いています"）
  bool chipPulse = false;  // 処理中はチップを点滅させる
  char label[28] = "";     // 右上のラベル（モデル名）
  int8_t wifiBars = -1;    // Wi-Fi の強さ 0〜3（-1 = 非表示）
  bool photo = false;      // 写真を添付している
  bool buttons = false;    // 下段に「話す」を出す
  bool cameraButton = false;  // 「見せて」も並べる
  bool meter = false;      // 下段に入力音量のメーターを出す
};

// タッチ領域。描画とタッチ判定で同じ座標を使う。
struct HudRect {
  int16_t x, y, w, h;
  bool contains(int16_t px, int16_t py) const {
    return px >= x && px < x + w && py >= y && py < y + h;
  }
};
constexpr HudRect kHudTalkButton = {6, 200, 196, 36};
constexpr HudRect kHudTalkButtonWide = {6, 200, 308, 36};  // カメラが使えないとき
constexpr HudRect kHudCameraButton = {208, 200, 106, 36};

// 顔スプライトに会話 UI を描く。
// m5avatar は 320x240 の 1bit スプライトへ顔パーツを描いてから LCD へ転送するので、
// 同じスプライトへ描けば描画タスク間で LCD を取り合わずに済む（ちらつきも出ない）。
class FaceHud {
 public:
  FaceHud();

  // 表示内容を更新する（メインループから）
  void set(const HudData& data);

  // 字幕を表示する。durationMs=0 で clearCaption() まで表示
  void setCaption(const String& text, uint32_t durationMs);
  void clearCaption();
  bool hasCaption();

  // 入力音量（0〜100）をメーターへ送る。録音タスクから呼んでよい
  void pushMeter(uint8_t level);

  // 短いステータス表示。字幕が無いときだけ下部に出す
  void setToast(const char* text, uint32_t durationMs);

  bool isVisible() const { return data_.visible; }

  // 顔スプライトへ描く（アバター描画タスクから）
  void draw(M5Canvas* canvas, int colorDepth, uint16_t fg, uint16_t bg);

 private:
  static constexpr int kMeterBars = 24;

  void drawTopBar(M5Canvas* c, const HudData& d, uint16_t fg, uint16_t bg,
                  uint32_t now);
  void drawButtons(M5Canvas* c, bool camera, uint16_t fg, uint16_t bg);
  void drawMeter(M5Canvas* c, const uint8_t* bars, int head, uint16_t fg);
  // UTF-8 テキストを幅 maxWidth で折り返して lines 行ぶん描く。
  // skipLines 行ぶんは描かずに読み飛ばす。戻り値は全行数。
  int drawWrapped(M5Canvas* c, const char* text, int x, int y, int maxWidth,
                  int lineHeight, int skipLines, int lines, uint16_t fg,
                  uint16_t bg);

  portMUX_TYPE mux_;
  HudData data_;
  uint8_t meter_[kMeterBars] = {};
  int meterHead_ = 0;
  char caption_[256] = "";
  uint32_t captionSince_ = 0;
  uint32_t captionUntil_ = 0;  // 0 = 無期限
  char toast_[40] = "";
  uint32_t toastUntil_ = 0;

  // 字幕の行数キャッシュ（描画タスクだけが触る）
  uint32_t cachedSince_ = UINT32_MAX;
  int cachedLines_ = 1;
  char cachedCaption_[256] = "";
  uint8_t lineStarts_[257] = {};  // byte offsets; caption is at most 255 bytes
};

// 顔の口パーツを包み、口を描いたあとに HUD を重ねる Drawable。
// Face には HUD 用の差し込み口が無いため、全顔テンプレートにある「口」を利用する。
class HudMouth : public m5avatar::Drawable {
 public:
  HudMouth(m5avatar::Drawable* inner, FaceHud* hud) : inner_(inner), hud_(hud) {}
  ~HudMouth() override { delete inner_; }

  void draw(M5Canvas* spi, m5avatar::BoundingRect rect,
            m5avatar::DrawContext* ctx) override;

 private:
  m5avatar::Drawable* inner_;
  FaceHud* hud_;
};
