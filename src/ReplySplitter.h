#pragma once

#include <cstdint>
#include <functional>
#include <string>

enum class Mood : uint8_t { Neutral, Happy, Sad, Angry, Doubt, Sleepy };

// Muse から少しずつ届く返事を、読み上げる文に切り分ける。
// 文頭の [happy] などの気持ちのタグを取り出し、読み上げに向かない記号・絵文字・URL を落とす。
// Arduino に依存しない（tools/reply_test で PC 上でもテストする）。
class ReplySplitter {
 public:
  using Emit = std::function<void(const std::string& sentence, Mood mood)>;

  explicit ReplySplitter(Emit emit) : emit_(std::move(emit)) {}

  void feed(const char* fragment);
  void finish();  // 最後の文を送り出す

 private:
  void drain(bool final);
  void emitSentence(std::string sentence);

  Emit emit_;
  std::string pending_;
  Mood mood_ = Mood::Neutral;
};

// 読み上げ・字幕に向かない文字を取り除いて前後の空白を落とす
std::string cleanForSpeech(const std::string& text);
