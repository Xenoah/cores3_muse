#include "JaDict.h"

#include <cstring>

#include "Kana.h"

namespace talk {

namespace {
constexpr int kCostOffset = 4000;  // make_dict.py の COST_OFFSET / COST_STEP
constexpr int kCostStep = 96;
}  // namespace

bool JaDict::open(const uint8_t* data, size_t size) {
  data_ = nullptr;
  if (data == nullptr || size < 64 || memcmp(data, "SCJD", 4) != 0 ||
      readU16(data + 4) != 1) {
    return false;
  }
  charCount_ = readU32(data + 8);
  charOffset_ = readU32(data + 12);
  const uint32_t typeCount = readU32(data + 16);
  const uint32_t typeOffset = readU32(data + 20);
  ruleCount_ = readU32(data + 24);
  ruleOffset_ = readU32(data + 28);
  connSize_ = readU16(data + 32);
  bosRight_ = data[34];
  eosLeft_ = data[35];
  connOffset_ = readU32(data + 36);
  const uint32_t blockCount = readU32(data + 40);
  const uint32_t blockIndex = readU32(data + 44);
  const uint32_t keyIndex = readU32(data + 48);
  const uint32_t keyArea = readU32(data + 52);
  const uint32_t dataOffset = readU32(data + 56);
  maxChars_ = readU16(data + 60);
  unkType_ = readU16(data + 62);
  if (dataOffset > size || typeOffset + typeCount * 8 > size) return false;

  types_.resize(typeCount);
  for (uint32_t i = 0; i < typeCount; ++i) {
    const uint8_t* t = data + typeOffset + i * 8;
    types_[i] = {t[0], t[1], t[2], t[3], (t[4] & 1) != 0, readU16(t + 6)};
  }
  blocks_.init(data, blockCount, blockIndex, keyIndex, keyArea, dataOffset);
  data_ = data;
  size_ = size;
  return true;
}

bool JaDict::encodeChar(uint32_t cp, std::string& out) const {
  if (cp > 0xFFFF) return false;
  int lo = 0;
  int hi = static_cast<int>(charCount_) - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    const uint8_t* e = data_ + charOffset_ + mid * 4;
    const uint16_t c = readU16(e);
    if (c == cp) {
      const uint16_t code = readU16(e + 2);
      if (code < 0x80) {
        out += static_cast<char>(code);
      } else {
        out += static_cast<char>(0x80 | (code >> 8));
        out += static_cast<char>(code & 0xFF);
      }
      return true;
    }
    if (c < cp) {
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return false;
}

bool JaDict::lookup(const std::string& key, const uint32_t* surface,
                    size_t surfaceLen, std::vector<JaEntry>& out) {
  const uint8_t* k = reinterpret_cast<const uint8_t*>(key.data());
  const int blockIndex = blocks_.findBlock(k, key.size());
  if (blockIndex < 0) {
    // 最初のブロックの先頭より前。先頭キーが key で始まるなら続ける
    size_t size = 0;
    const uint8_t* b = blocks_.block(0, &size);
    if (b == nullptr || size < 2) return false;
    return b[1] >= key.size() && memcmp(b + 2, k, key.size()) == 0;
  }

  // ブロック内を先頭から読み、一致する語と、その次の語（前方一致の判定用）を探す
  for (uint32_t bi = static_cast<uint32_t>(blockIndex); bi < blocks_.blockCount(); ++bi) {
    size_t size = 0;
    const uint8_t* b = blocks_.block(bi, &size);
    if (b == nullptr) return false;
    size_t pos = 0;
    uint8_t current[256];
    size_t currentLen = 0;
    while (pos + 2 <= size) {
      const uint8_t prefix = b[pos];
      const uint8_t suffix = b[pos + 1];
      pos += 2;
      if (prefix > currentLen || pos + suffix > size) return false;
      memcpy(current + prefix, b + pos, suffix);
      currentLen = prefix + suffix;
      pos += suffix;
      const uint8_t count = b[pos++];
      const int cmp = BlockStore::compareKeys(current, currentLen, k, key.size());
      for (uint8_t e = 0; e < count; ++e) {
        uint16_t type = b[pos++];
        if (type & 0x80) type = ((type & 0x7F) << 8) | b[pos++];
        const uint8_t accent = b[pos++];
        const uint8_t costQ = b[pos++];
        const uint8_t copy = b[pos++];
        const uint8_t explicitLen = b[pos++];
        if (cmp == 0) {
          JaEntry entry;
          entry.type = type;
          entry.accent = accent;
          entry.cost = static_cast<int16_t>(costQ * kCostStep - kCostOffset);
          for (uint8_t i = 0; i < explicitLen; ++i) {
            const uint8_t r = b[pos + i];
            appendUtf8(entry.pron, r == 0x60 ? 0x2019 : 0x30A0 + r);
          }
          const size_t n = surfaceLen;
          for (size_t i = n - (copy < n ? copy : n); i < n; ++i) {
            appendUtf8(entry.pron, hiraToKata(surface[i]));
          }
          out.push_back(entry);
        }
        pos += explicitLen;
      }
      if (cmp > 0) {
        // key より後ろの最初の語。key で始まっていれば、より長い一致があり得る
        return currentLen > key.size() && memcmp(current, k, key.size()) == 0;
      }
    }
    // ブロックの最後まで key 以下だった → 次のブロックの先頭で判定する
  }
  return false;
}

void JaDict::rule(uint16_t ruleId, uint8_t prevPos, uint8_t* code,
                  int8_t* add) const {
  *code = kRuleNone;
  *add = 0;
  if (ruleId >= ruleCount_) return;
  const uint8_t* r = data_ + ruleOffset_ + readU32(data_ + ruleOffset_ + ruleId * 4);
  const uint8_t n = r[0];
  bool haveDefault = false;
  for (uint8_t i = 0; i < n; ++i) {
    const uint8_t pos = r[1 + i * 3];
    const uint8_t c = r[2 + i * 3];
    const int8_t a = static_cast<int8_t>(r[3 + i * 3]);
    if (pos == prevPos) {
      *code = c;
      *add = a;
      return;
    }
    if (pos == 0xFF && !haveDefault) {
      *code = c;
      *add = a;
      haveDefault = true;
    }
  }
}

int16_t JaDict::connCost(uint8_t rightCluster, uint8_t leftCluster) const {
  const uint8_t* p = data_ + connOffset_ + (rightCluster * connSize_ + leftCluster) * 2;
  return static_cast<int16_t>(readU16(p));
}

}  // namespace talk
