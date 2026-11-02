#pragma once

// 辞書の「昇順に並んだキーを deflate で固めたブロック」を読む共通部分（ja.dic / en.dic）。
// ブロックの先頭キーは非圧縮で持っているので、二分探索でブロックを決めてから展開する。
// 展開したブロックは数個だけキャッシュする（続けて近いキーを引くことが多いため）。

#include <cstddef>
#include <cstdint>
#include <vector>

namespace talk {

// raw deflate を展開する（ESP32-S3 は ROM の tinfl、PC は miniz）。成功で true
bool inflateRaw(const uint8_t* in, size_t inSize, uint8_t* out, size_t outSize);

// 大きめの作業領域を確保する（ESP32 では PSRAM。TLS 用の内部 RAM を使わないため）
void* allocLarge(size_t size);
void freeLarge(void* p);

class BlockStore {
 public:
  // index: 8 bytes/ブロック [u32 データ位置, u16 圧縮長, u16 展開長]
  // keyIndex: u32/ブロック（keyArea 内の位置）、keyArea: [u8 長さ][キー]
  void init(const uint8_t* base, uint32_t blockCount, uint32_t indexOffset,
            uint32_t keyIndexOffset, uint32_t keyAreaOffset, uint32_t dataOffset);

  uint32_t blockCount() const { return blockCount_; }

  // キーが入っているはずのブロック（先頭キー <= key の最後のブロック）。無ければ -1
  int findBlock(const uint8_t* key, size_t len) const;

  // ブロックを展開して返す（キャッシュつき）。失敗で nullptr
  const uint8_t* block(uint32_t index, size_t* size);

  static int compareKeys(const uint8_t* a, size_t alen, const uint8_t* b,
                         size_t blen);

 private:
  struct Slot {
    int32_t block = -1;
    uint32_t usedAt = 0;
    uint8_t* data = nullptr;
    size_t capacity = 0;
    size_t size = 0;
  };

  const uint8_t* base_ = nullptr;
  uint32_t blockCount_ = 0;
  uint32_t indexOffset_ = 0;
  uint32_t keyIndexOffset_ = 0;
  uint32_t keyAreaOffset_ = 0;
  uint32_t dataOffset_ = 0;
  Slot slots_[6];
  uint32_t clock_ = 0;
};

// リトルエンディアンの読み出し
inline uint16_t readU16(const uint8_t* p) { return p[0] | (p[1] << 8); }
inline uint32_t readU32(const uint8_t* p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace talk
