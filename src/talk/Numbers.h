#pragma once

// 数の読み（日本語・英語）と、数のあとの助数詞の音の変化（1分 → イップン、3本 → サンボン）。

#include <cstdint>
#include <string>
#include <vector>

namespace talk {

// 日本語の数の読み。部品ごとに分けて持つ（助数詞で最後の部品が変わるため）
struct NumberReading {
  std::vector<std::string> parts;  // 例: 25 → {"ニ", "ジュウ", "ゴ"}
  uint32_t lastUnit = 0;           // 最後の部品の値（1〜9, 10, 100, 1000, 10000, …、0 = ゼロ）
  uint8_t accent = 0;              // 句のアクセント（最初の部品の型）

  std::string pron() const;
};

// 数字列（'0'〜'9'、小数点 '.' を1つまで）を日本語で読む。
// 0 で始まる2桁以上・17桁以上は1桁ずつ読む（電話番号など）
NumberReading readNumberJa(const std::string& digits);

// 漢数字（〇一二三四五六七八九十百千万億兆）を数字列にする。読めなければ空
std::string kanjiNumeralToDigits(const std::u32string& text);
bool isKanjiNumeral(uint32_t c);

// 数のあとの助数詞の読み。counter が表にあれば true を返し、
// number の最後の部品と counterPron を書き換える（例: 1 + 分 → イッ + プン）
bool applyCounter(const std::u32string& counter, NumberReading& number,
                  std::string& counterPron);

// 英語の数（英字に続く数字: "3D" → スリー、"P100" → ワンハンドレッド）
std::string readNumberEn(const std::string& digits);

}  // namespace talk
