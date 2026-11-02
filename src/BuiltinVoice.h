#pragma once

#include <Arduino.h>
#include <FS.h>

#include <string>
#include <vector>

#include "VoiceVoxClient.h"  // LipSyncCallback / ServiceCallback
#include "talk/SpeechSynth.h"

// 内蔵ボイス（TTS サーバーなしで喋るためのオフライン音声）。
//
// tools/make_voice_pack.py が VOICEVOX で作ったボイスパック（LittleFS の /voice.pak）を使う。
//   1. 実況のように「セリフ断片・数字・数字＋単位」のクリップで全部読める文は、
//      クリップを最長一致でつなげて読む（自然な抑揚）
//   2. それ以外の文（Web から送った文章・英語・ジョブ名など）は、ファームウェアに埋め込んだ
//      辞書（dict/ja.dic・en.dic）で読みとアクセントを付け、1モーラずつの音（低・高・無声）を
//      つなげて読む（src/talk/）
// 古いボイスパック（モーラの音が無い）では 2 ができないので、読めない部分は飛ばし、
// 「」で囲まれたジョブ名は「作品」と読み替える。
class BuiltinVoice {
 public:
  // LittleFS をマウントしてボイスパックの索引を読み込み、読み辞書を開く。
  // 戻り値: 使える=true（パックが無ければ false、エラー内容は lastError()）
  bool begin();

  void setCallbacks(LipSyncCallback lipSync, ServiceCallback service);

  bool isReady() const { return ready_; }
  // どんな文章でも読める（モーラの音と読み辞書がそろっている）
  bool canReadAnything() const { return ready_ && moraReady_; }
  uint16_t clipCount() const { return count_; }
  uint32_t sampleRate() const { return rate_; }
  const String& lastError() const { return lastError_; }

  // 文章を内蔵ボイスで読み上げる（ブロッキング、口パク付き）。
  // 読めるクリップが1つも無ければ何もせず false を返す。
  bool speak(const String& text);

  // 再生中の speak() をできるだけ早く終わらせる（service コールバックから呼んでよい）
  void abort() { abort_ = true; }

 private:
  struct Clip {
    uint32_t keyOffset;   // index_ 内のキー位置
    uint32_t dataOffset;  // データ部先頭からの位置
    uint32_t samples;
    int16_t predictor;    // ADPCM の初期値
    uint8_t keyLen;
    uint8_t stepIndex;
  };

  struct Step {
    int16_t clip;      // -1 = 無音
    uint16_t pauseMs;
    bool join;         // 直前のクリップと重ねてつなぐ（モーラどうし）
    bool mora = false;
    talk::MoraProsody prosody;
  };

  const char* key(const Clip& clip) const { return index_ + clip.keyOffset; }
  int findKey(const char* text, size_t len) const;
  int longestPlainKeyAt(const char* text, size_t remaining, size_t* matched) const;

  // セリフのクリップで読む。読めなかった文字数を返す
  size_t planPhrases(const std::string& text, std::vector<Step>& steps) const;
  size_t planNumber(const char* s, size_t n, size_t begin, size_t end,
                    std::vector<Step>& steps) const;
  // 辞書とモーラの音で読む
  void planSpeech(const std::string& text, std::vector<Step>& steps) const;
  void plan(const String& text, std::vector<Step>& steps);
  void addClip(std::vector<Step>& steps, int clip, bool join = false) const;
  void addPause(std::vector<Step>& steps, uint16_t ms) const;

  bool playClip(const Clip& clip, bool join, bool holdTail);
  bool playMora(const Step& step, bool holdTail, talk::MoraRenderer& renderer);
  bool decodeSamples(int16_t* out, size_t count, int& predictor, int& stepIndex);
  void playSilence(uint16_t ms);
  void write(const int16_t* samples, size_t count);
  void flushTail();
  void flushBuffer();
  void submit(size_t sampleCount);

  bool ready_ = false;
  bool moraReady_ = false;
  uint8_t codec_ = 1;      // 0 = IMA-ADPCM, 1 = μ-law
  uint32_t rate_ = 12000;
  uint16_t count_ = 0;
  uint32_t dataStart_ = 0;
  char* index_ = nullptr;  // キー文字列を含む索引（PSRAM）
  Clip* clips_ = nullptr;  // キーのバイト順に並んだクリップ（PSRAM）
  uint16_t bucket_[257] = {};  // 先頭バイトごとのクリップ範囲
  File file_;
  size_t bufferIndex_ = 0;
  size_t bufferFill_ = 0;      // 書き込み中のバッファに入っているサンプル数
  size_t tailLen_ = 0;         // 次のモーラと重ねるために取っておいた末尾
  uint32_t maxMoraUs_ = 0;
  LipSyncCallback lipSync_ = nullptr;
  ServiceCallback service_ = nullptr;
  String lastError_;
  volatile bool abort_ = false;
};
