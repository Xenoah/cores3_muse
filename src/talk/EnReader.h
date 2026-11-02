#pragma once

// 英単語 → カタカナ読み（日本語話者が英語を読むときの読み）とアクセント。
//
//   1. 英語辞書（dict/en.dic、CMU 発音辞書の頻出語）で発音記号を引く
//   2. 無ければ綴りから発音を推定する（辞書から学習した規則、en.dic に同梱）
//   3. ローマ字として読める語（zundamon, sushi）はローマ字読み
//   4. 大文字だけの短い語・子音だけの語（AMS, PLA, HMS）はアルファベット読み
// 発音記号（ARPAbet）は規則でカタカナにする（例: K AE1 T → キャット）。
// 強勢のある母音のモーラにアクセント核を置く。

#include <cstdint>
#include <string>
#include <vector>

#include "BlockStore.h"
#include "JaReader.h"

namespace talk {

// 英語辞書（tools/make_dict.py が作る en.dic）
class EnDict {
 public:
  bool open(const uint8_t* data, size_t size);
  bool isOpen() const { return data_ != nullptr; }

  // 小文字の単語の発音（音素コードの列）を引く
  bool lookup(const std::string& word, std::vector<uint8_t>& phones);

  // 綴りから発音を推定する（学習済みの決定木）。モデルが無ければ false
  bool guess(const std::string& word, std::vector<uint8_t>& phones) const;

 private:
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
  uint32_t ltsOffset_ = 0;
  uint32_t ltsSize_ = 0;
  BlockStore blocks_;
};

class EnReader {
 public:
  void setDict(EnDict* dict) { dict_ = dict; }

  // 英字と数字の並び（1語）を読んで out に足す（キャメルケース・英字と数字の境目で分ける）
  void read(const std::string& word, std::vector<JaToken>& out);

  // アルファベット読み（"AMS" → エーエムエス）
  static std::string spell(const std::string& letters);

  // 音素コード列 → カタカナ（accent に強勢のモーラの位置、無ければ 0）。
  // oForAA: 綴りが o の AA を「オ」にする（hot → ホット。father はア）
  static std::string phonesToKana(const std::vector<uint8_t>& phones, uint8_t* accent,
                                  bool oForAA = false);

  // ローマ字として読めればカタカナにする（読めなければ空）
  static std::string romajiToKana(const std::string& lower);

 private:
  void readPart(const std::string& part, std::vector<JaToken>& out);

  EnDict* dict_ = nullptr;
};

}  // namespace talk
