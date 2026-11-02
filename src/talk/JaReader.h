#pragma once

// 日本語の文を読み（カタカナ）とアクセント句にする。
//
// 1. 辞書のラティスを作り、MeCab と同じ考え方（単語コスト＋連接コスト）の Viterbi で
//    いちばんもっともらしい区切りと読みを選ぶ（連接コストは 256 クラスタに縮約したもの）
// 2. Open JTalk の規則（njd_set_accent_phrase / njd_set_accent_type）にならって
//    単語をアクセント句にまとめ、句のアクセント核を決める
// 3. 母音の無声化（です・ます、無声子音にはさまれたイ・ウ）を付ける

#include <cstdint>
#include <string>
#include <vector>

#include "JaDict.h"
#include "Kana.h"

namespace talk {

// 読みを決めた1語（数字・英単語もこの形で句作りに混ぜる）
struct JaToken {
  std::u32string surface;
  std::string pron;  // カタカナ（UTF-8、無声化は ’）
  uint8_t accent = 0;
  uint8_t pos = kPosNoun;
  uint8_t group = kGroupOther;
  bool renyou = false;
  uint16_t rule = 0xFFFF;  // 0xFFFF = 規則なし（数字・英単語は名詞の複合 C1 相当）
  uint8_t fixedRule = kRuleC1;
  bool latin = false;      // 英単語（英単語どうしは別の句にする）
  bool number = false;     // 数（助数詞の読みの変化に使う）
  std::string digits;      // 数の値（"130000000" や "3.14"）
};

class JaReader {
 public:
  explicit JaReader(JaDict& dict) : dict_(dict) {}

  // 直前の文脈（ラティスの文頭につなぐ右文脈）
  enum class Context : uint8_t {
    Start,   // 文頭・句読点のあと
    Number,  // 数のあと（「3時」の「時」を助数詞として読ませる）
    Noun,    // 英単語などの名詞のあと（「PETGと」の「と」を助詞として読ませる）
  };

  // ひらがな・カタカナ・漢字の並びを単語に分けて読みを付け、out に足す。
  void tokenize(const std::u32string& text, Context context,
                std::vector<JaToken>& out);

  // 単語列をアクセント句にして utterance に足す（最後の句の pauseAfterMs は呼び出し側で決める）
  void buildPhrases(const std::vector<JaToken>& tokens, Utterance& utterance);

 private:
  struct Node {
    int start;
    int end;
    JaEntry entry;
    int32_t best;
    int prev;  // -1 = 文頭
  };

  void addUnknown(const std::u32string& text, int start, int end,
                  int32_t cost, std::vector<Node>& nodes);
  bool chains(const JaToken& prev, const JaToken& cur) const;
  void applyRule(const JaToken& prev, const JaToken& cur, int moraSize,
                 int* accent) const;

  JaDict& dict_;
};

// 母音の無声化を付ける（句をまたいで判定する）
void markDevoicing(Utterance& utterance);

}  // namespace talk
