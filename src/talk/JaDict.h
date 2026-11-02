#pragma once

// 日本語の読み辞書（dict/ja.dic、tools/make_dict.py が作る）を引く。
// 形式は make_dict.py の説明を参照。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "BlockStore.h"

namespace talk {

// 品詞（make_dict.py の POS_CODES と同じ）
enum JaPos : uint8_t {
  kPosOther = 0,
  kPosNoun = 1,
  kPosVerb = 2,
  kPosAdjective = 3,
  kPosAdverb = 4,
  kPosParticle = 5,
  kPosAuxiliary = 6,
  kPosConjunction = 7,
  kPosAdnominal = 8,
  kPosPrefix = 9,
  kPosInterjection = 10,
  kPosFiller = 11,
  kPosSymbol = 12,
};

// 品詞細分類1（G1_CODES と同じ）
enum JaGroup : uint8_t {
  kGroupOther = 0,
  kGroupAdjectivalNoun = 1,  // 形容動詞語幹
  kGroupAdverbial = 2,       // 副詞可能
  kGroupSuffix = 3,          // 接尾
  kGroupDependent = 4,       // 非自立
  kGroupSahen = 5,           // サ変接続
  kGroupConjunctive = 6,     // 接続助詞
  kGroupNumber = 7,          // 数
  kGroupProper = 8,
  kGroupPronoun = 9,
  kGroupIndependent = 10,
};

// アクセント結合規則（RULE_CODES と同じ）
enum JaRule : uint8_t {
  kRuleNone = 0, kRuleF1, kRuleF2, kRuleF3, kRuleF4, kRuleF5,
  kRuleC1, kRuleC2, kRuleC3, kRuleC4, kRuleC5,
  kRuleP1, kRuleP2, kRuleP6, kRuleP14,
};

struct JaType {
  uint8_t leftCluster;
  uint8_t rightCluster;
  uint8_t pos;
  uint8_t group;
  bool renyou;  // 活用形が連用〜
  uint16_t rule;
};

struct JaEntry {
  uint16_t type;
  uint8_t accent;
  int16_t cost;
  std::string pron;  // カタカナ（UTF-8、無声化は ’）
};

class JaDict {
 public:
  bool open(const uint8_t* data, size_t size);
  bool isOpen() const { return data_ != nullptr; }

  // 文字を辞書の文字コード（1〜2 バイト）にする。辞書に無い文字は false
  bool encodeChar(uint32_t cp, std::string& out) const;

  // 表層形（encodeChar したバイト列）に一致する項目を out に足す。
  // surface は読みの末尾コピー用（UTF-8 ではなくコードポイント列）。
  // 戻り値: key で始まる、より長いキーが辞書にあるか（前方一致の探索を続けるか）
  bool lookup(const std::string& key, const uint32_t* surface, size_t surfaceLen,
              std::vector<JaEntry>& out);

  const JaType& type(uint16_t id) const { return types_[id]; }
  // 前の語の品詞に合う規則（規則コード, 加算）を返す
  void rule(uint16_t ruleId, uint8_t prevPos, uint8_t* code, int8_t* add) const;
  int16_t connCost(uint8_t rightCluster, uint8_t leftCluster) const;
  uint16_t unknownType() const { return unkType_; }
  uint8_t bosRightCluster() const { return bosRight_; }
  uint8_t eosLeftCluster() const { return eosLeft_; }
  uint16_t maxChars() const { return maxChars_; }

 private:
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
  uint32_t charCount_ = 0;
  uint32_t charOffset_ = 0;
  uint32_t ruleCount_ = 0;
  uint32_t ruleOffset_ = 0;
  uint16_t connSize_ = 0;
  uint32_t connOffset_ = 0;
  uint16_t maxChars_ = 0;
  uint16_t unkType_ = 0;
  uint8_t bosRight_ = 0;
  uint8_t eosLeft_ = 0;
  std::vector<JaType> types_;
  BlockStore blocks_;
};

}  // namespace talk
