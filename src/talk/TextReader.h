#pragma once

// 文章 → 読み（アクセント句の並び）。本体の内蔵ボイスで任意の文章を喋るための入口。
//
//   - 全角英数・半角カナをそろえる
//   - 日本語（ひらがな・カタカナ・漢字）は JaReader で読みとアクセント
//   - 数字は日本語の数として読む（3時・1分・2人 などの助数詞の音の変化つき）
//   - 英字は EnReader（英単語の発音をカタカナ読みに。略語はアルファベット読み）
//   - % ℃ & + = などの記号は読み、句読点は間（ま）にする

#include <string>

#include "EnReader.h"
#include "JaDict.h"
#include "JaReader.h"
#include "Kana.h"

namespace talk {

class TextReader {
 public:
  TextReader(JaDict& ja, EnReader& en) : ja_(ja), jaReader_(ja), en_(en) {}

  // 文章を読んで utterance に足す
  void read(const std::string& text, Utterance& utterance);

 private:
  void flush(std::vector<JaToken>& tokens, Utterance& utterance,
             uint16_t pauseMs, bool question);
  void addJapanese(const std::u32string& run, std::vector<JaToken>& tokens);

  JaDict& ja_;
  JaReader jaReader_;
  EnReader& en_;
};

// 全角英数・全角記号を半角に、半角カナを全角にする（NFKC の一部）
std::u32string normalizeText(const std::u32string& text);

}  // namespace talk
