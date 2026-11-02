#pragma once

#include <Arduino.h>
#include <Avatar.h>
#include <faces/FaceTemplates.hpp>

#include "FaceHud.h"

// M5Stack-Avatarライブラリのラッパークラス。
// 表情・顔型・カラーパレット・目パターン・変形パターンを管理し、
// 自動まばたき・ショーケース自動巡回・ステータステキスト表示機能を提供する。
class AvatarFaceController {
 public:
  // 目の動きパターン
  enum class EyePattern : uint8_t {
    AutoBlink, // 3〜10秒ランダム間隔で自動まばたき
    Open,      // 両目を常に開く
    WinkLeft,  // 左目を閉じる（右目から見た視点）
    WinkRight, // 右目を閉じる
    Closed     // 両目を閉じる
  };

  // 顔のスケール・傾き変形パターン
  enum class TransformPattern : uint8_t {
    Normal,    // 通常（変形なし）
    ZoomIn,    // 1.15倍に拡大
    ZoomOut,   // 0.85倍に縮小
    TiltLeft,  // 左に傾ける（-0.12 rad）
    TiltRight  // 右に傾ける（+0.12 rad）
  };

  // アバターを初期化し、描画タスクを起動する（setup()から1回だけ呼ぶ）
  void begin();

  // ステータス表示のクリア・ショーケース進行・まばたきを更新する（毎フレーム呼ぶ）
  void update();

  // 表情を指定した種類に設定する（6種類: Happy/Angry/Sad/Doubt/Sleepy/Neutral）
  void setExpression(m5avatar::Expression expression);

  // 次の表情に切り替え、ステータスに名前を表示する
  void nextExpression();

  // 次の顔型に切り替える（7種類: Default/Simple/Omega/Girly/Girly2/PinkDemon/Doggy）
  void nextFace();

  // カラーパレットを切り替える（5種類: Default/Skin/Cyber/Mono/Demon）
  // direction: +1=次へ、-1=前へ
  void nextPalette(int direction = 1);

  // 次の目パターンに切り替える（5種類）
  void nextEyePattern();

  // 次の変形パターンに切り替える（5種類）
  void nextTransform();

  // 口の開き具合を設定する（0.0=閉じる〜1.0=全開）。TTS再生中の口パクに使う。
  void setMouthOpenRatio(float ratio);

  // 視線方向を設定する（-1.0〜+1.0、両目同時）
  void setGaze(float vertical, float horizontal);

  // ゲーミングRGB（顔を虹色にゆっくり循環させる演出）のON/OFFを切り替える。
  // OFFにすると現在のカラーパレットを即座に再適用して通常表示に戻す。
  void setGamingRgb(bool enabled);

  // ゲーミングRGBが有効かどうか
  bool isGamingRgb() const;

  // 現在のゲーミング虹色フェーズ（0.0〜1.0）を返す。本体LEDと色を合わせるのに使う。
  float gamingHue() const;

  // 顔の2色（RGB565）を直接指定する。会話の状態を色で伝えるのに使う。
  void setColors(uint16_t background, uint16_t foreground);

  // ステータステキストを表示する。durationMs=0で常時表示。
  // HUD 表示中は吹き出しの代わりに HUD 下段へ短く表示する。
  void showStatus(const char* text, uint32_t durationMs = 1400);

  // 日本語字幕。全モードで表示し、durationMs=0なら発話終了まで保持できる。
  void showCaption(const String& text, uint32_t durationMs);

  // 顔に重ねるプリンタ HUD（全顔テンプレートの口パーツに差し込み済み）
  FaceHud& hud();

  // HUD の表示 ON/OFF。表示中は呼吸ズームを止めて HUD が揺れないようにする。
  void setHudVisible(bool visible);

  // アバターの描画タスク（FreeRTOS）を一時停止する。
  // メニュー表示などで画面を直接描画する前に呼ぶ。描きかけのフレームは描き終えるのを
  // 待ってから止める（LCD の SPI バスをつかんだまま止めないため）。
  // 止めている間は表情の変更を保留し、resumeDrawing() でまとめて反映する。
  void pauseDrawing();

  // アバターの描画タスクを再開する
  void resumeDrawing();

  // 描画タスクを止めている間は true
  bool isDrawingPaused() const { return drawingPaused_; }

  // 顔の描画タスクのスタックの残り（今までで最も少なかったときのバイト数。監視用）
  uint32_t drawStackFree() const;

  // 全パラメータをデフォルト（Neutral表情・Default顔・Default色）に戻す
  void resetToDefault();

  // 指定ミリ秒後にresetToDefault()を自動実行するタイマーをセットする
  void returnToDefaultAfter(uint32_t delayMs);

  // ショーケースモードのON/OFFを切り替える。
  // ON時は1.8秒ごとに表情・目・変形を自動サイクルする。
  void toggleShowcase();

  // ショーケースモードが有効かどうか
  bool isShowcaseEnabled() const;

  // 現在の表情名を返す（例: "HAPPY"）
  const char* expressionName() const;

  // 現在の顔型名を返す（例: "DEFAULT"）
  const char* faceName() const;

  // 現在のパレット名を返す（例: "CYBER"）
  const char* paletteName() const;

  // 現在の目パターン名を返す（例: "AUTO BLINK"）
  const char* eyePatternName() const;

  // 現在の変形パターン名を返す（例: "ZOOM IN"）
  const char* transformName() const;

 private:
  // 各パターンの総数
  static constexpr size_t kExpressionCount = 6;
  static constexpr size_t kFaceCount = 7;
  static constexpr size_t kPaletteCount = 5;
  static constexpr size_t kEyePatternCount = 5;
  static constexpr size_t kTransformPatternCount = 5;

  m5avatar::Avatar avatar_;                     // アバター本体（描画・アニメーション管理）
  m5avatar::Face* faces_[kFaceCount] = {};      // 顔型オブジェクトの配列
  m5avatar::ColorPalette palettes_[kPaletteCount]; // カラーパレットの配列
  m5avatar::ColorPalette gamingPalette_;        // ゲーミングRGB用の動的パレット
  m5avatar::ColorPalette statePalette_;         // setColors() 用のパレット
  FaceHud hud_;                                 // プリンタ HUD（口パーツ経由で描画）

  size_t expressionIndex_ = 5;        // 現在の表情インデックス（5=Neutral）
  size_t faceIndex_ = 0;              // 現在の顔型インデックス（0=Default）
  size_t paletteIndex_ = 0;           // 現在のパレットインデックス（0=Default）
  size_t eyePatternIndex_ = 0;        // 現在の目パターンインデックス（0=AutoBlink）
  size_t transformPatternIndex_ = 0;  // 現在の変形パターンインデックス（0=Normal）

  bool started_ = false;          // begin()が呼ばれたかどうか
  bool drawingPaused_ = false;    // 描画タスクが一時停止中かどうか
  bool expressionPending_ = false; // 一時停止中に変えた表情（再開時に反映する）
  bool captionVisible_ = false;   // 字幕表示中はズーム・回転を止める
  bool showcaseEnabled_ = false;  // ショーケースモードが有効かどうか
  bool blinkClosed_ = false;      // まばたき中（目を閉じている）かどうか
  bool gamingRgb_ = false;        // ゲーミングRGB（虹色循環）が有効かどうか
  uint32_t lastGamingUpdateAt_ = 0; // 最後に虹色パレットを更新した時刻（throttle用）
  float gamingHue_ = 0.0f;        // 現在の虹色フェーズ（0.0〜1.0）
  uint32_t nextShowcaseAt_ = 0;   // 次のショーケース更新時刻（millis）
  uint32_t statusClearAt_ = 0;    // ステータステキストを消す時刻（0=消さない）
  uint32_t defaultReturnAt_ = 0;  // デフォルトに戻る時刻（0=タイマーなし）
  uint32_t nextBlinkAt_ = 0;      // 次にまばたきを開始する時刻
  uint32_t blinkOpenAt_ = 0;      // まばたき後に目を開ける時刻

  // m5stack-avatar の描画タスク（スタック 2KB 固定）を、スタックの大きい自前のタスクに差し替える
  bool replaceDrawTask();

  // 7種類の顔型オブジェクトを生成してfaces_[]に格納する
  void initializeFaces();

  // 5種類のカラーパレットを設定してpalettes_[]に格納する
  void initializePalettes();

  // expressionIndex_に対応する表情をアバターに適用する
  void applyExpression();

  // 吹き出しの String を描画中に書き換えない。
  void applySpeechText(const char* text);

  // faceIndex_に対応する顔型をアバターに適用する
  void applyFace();

  // paletteIndex_に対応するカラーパレットをアバターに適用する
  void applyPalette();

  // eyePatternIndex_に対応する目パターンをアバターに適用する
  void applyEyePattern();

  // transformPatternIndex_に対応する変形をアバターに適用する
  void applyTransform();

  // ゲーミングRGB有効時に、虹色パレットを更新してアバターへ適用する（throttle付き）
  void updateGamingPalette(uint32_t now);

  // ショーケースモードで次のパターンに進める
  void advanceShowcase();

  // AutoBlinkモードのまばたきアニメーションを更新する
  void updateBlink(uint32_t now);

  // 次のまばたき開始時刻をランダムにスケジュールする（3〜10秒後）
  void scheduleNextBlink(uint32_t now);
};
