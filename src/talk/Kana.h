#pragma once

// 読み上げの共通部品（UTF-8・カタカナ・モーラ・アクセント句）。
// src/talk/ は Arduino に依存しない（PC でもビルドしてテストできる）。

#include <cstdint>
#include <string>
#include <vector>

namespace talk {

// UTF-8 を1文字読む（不正なバイトは U+FFFD にして1バイト進める）
uint32_t decodeUtf8(const std::string& s, size_t& pos);
void appendUtf8(std::string& out, uint32_t cp);
std::u32string toU32(const std::string& s);
std::string toUtf8(const std::u32string& s);

// 文字の種類
bool isHiragana(uint32_t c);
bool isKatakana(uint32_t c);  // 長音符「ー」を含む
bool isKanji(uint32_t c);     // 々〆ヶ を含む
bool isKanaSmall(uint32_t c); // ァィゥェォャュョヮ（モーラの後ろにつく小書き文字）
uint32_t hiraToKata(uint32_t c);

// 1モーラ（カタカナ1〜2文字。長音「ー」・促音「ッ」・撥音「ン」も1モーラ）
struct Mora {
  std::string kana;     // UTF-8
  bool devoiced = false;
};

// アクセント句（東京方言の高低。accent=0 は平板、n は n 番目のモーラの後で下がる）
struct AccentPhrase {
  std::vector<Mora> moras;
  int accent = 0;
  uint16_t pauseAfterMs = 0;  // この句のあとの間（0 = つなげて読む）
  bool question = false;      // 文末が「？」
};

using Utterance = std::vector<AccentPhrase>;

// カタカナ（UTF-8、無声化の印「’」を含んでよい）をモーラに分ける
void splitMoras(const std::string& katakana, std::vector<Mora>& out);

// ボイスパックの音にそろえたモーラ（ヲ → オ、ヂ → ジ、ヅ → ズ、ヵ → カ など）
std::string playableMora(const std::string& kana);

// モーラの母音（'a','i','u','e','o'。ン・ッ・ー は 0）
char moraVowel(const std::string& kana);

// 母音のカタカナ（'a' → "ア"）
const char* vowelKana(char vowel);

// モーラの子音が無声か（無声化の判定用）
bool moraVoicelessConsonant(const std::string& kana);

// i 番目（0 始まり）のモーラが高いか（アクセント型と句頭の低さから）
bool moraIsHigh(const AccentPhrase& phrase, size_t index);

// VOICEVOX（AquesTalk 風）記法にする。テスト用。
//   句の区切り "/"、間 "、"、アクセント核 "'"、無声化 "_"、長音は母音に展開
std::string toNotation(const Utterance& utterance);

}  // namespace talk
