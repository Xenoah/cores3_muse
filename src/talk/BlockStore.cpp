#include "BlockStore.h"

#include <cstdlib>
#include <cstring>

#if defined(ESP_PLATFORM)
#include "esp32s3/rom/miniz.h"
#include <esp_heap_caps.h>
#else
#include "miniz.h"  // PC でのテスト用（tools/talk_test が用意する）
#endif

namespace talk {

void* allocLarge(size_t size) {
#if defined(ESP_PLATFORM)
  void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
#else
  return malloc(size);
#endif
}

void freeLarge(void* p) {
#if defined(ESP_PLATFORM)
  heap_caps_free(p);
#else
  free(p);
#endif
}

bool inflateRaw(const uint8_t* in, size_t inSize, uint8_t* out, size_t outSize) {
  // tinfl_decompressor は約 11KB あるので1つだけ確保して使い回す
  static tinfl_decompressor* decomp = nullptr;
  if (decomp == nullptr) {
    decomp = static_cast<tinfl_decompressor*>(allocLarge(sizeof(tinfl_decompressor)));
    if (decomp == nullptr) return false;
  }
  tinfl_init(decomp);
  size_t inLen = inSize;
  size_t outLen = outSize;
  const tinfl_status status =
      tinfl_decompress(decomp, in, &inLen, out, out, &outLen,
                       TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  return status == TINFL_STATUS_DONE && outLen == outSize;
}

void BlockStore::init(const uint8_t* base, uint32_t blockCount,
                      uint32_t indexOffset, uint32_t keyIndexOffset,
                      uint32_t keyAreaOffset, uint32_t dataOffset) {
  base_ = base;
  blockCount_ = blockCount;
  indexOffset_ = indexOffset;
  keyIndexOffset_ = keyIndexOffset;
  keyAreaOffset_ = keyAreaOffset;
  dataOffset_ = dataOffset;
  for (Slot& s : slots_) s.block = -1;
}

int BlockStore::compareKeys(const uint8_t* a, size_t alen, const uint8_t* b,
                            size_t blen) {
  const size_t n = alen < blen ? alen : blen;
  const int c = n ? memcmp(a, b, n) : 0;
  if (c != 0) return c;
  if (alen == blen) return 0;
  return alen < blen ? -1 : 1;
}

int BlockStore::findBlock(const uint8_t* key, size_t len) const {
  int lo = 0;
  int hi = static_cast<int>(blockCount_) - 1;
  int found = -1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    const uint8_t* k =
        base_ + keyAreaOffset_ + readU32(base_ + keyIndexOffset_ + mid * 4);
    if (compareKeys(k + 1, k[0], key, len) <= 0) {
      found = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return found;
}

const uint8_t* BlockStore::block(uint32_t index, size_t* size) {
  if (index >= blockCount_) return nullptr;
  ++clock_;
  for (Slot& s : slots_) {
    if (s.block == static_cast<int32_t>(index)) {
      s.usedAt = clock_;
      *size = s.size;
      return s.data;
    }
  }
  Slot* victim = &slots_[0];
  for (Slot& s : slots_) {
    if (s.block < 0) {
      victim = &s;
      break;
    }
    if (s.usedAt < victim->usedAt) victim = &s;
  }
  const uint8_t* entry = base_ + indexOffset_ + index * 8;
  const uint32_t offset = readU32(entry);
  const uint16_t packed = readU16(entry + 4);
  const uint16_t raw = readU16(entry + 6);
  if (victim->capacity < raw) {
    freeLarge(victim->data);
    victim->capacity = raw < 4096 ? 4096 : raw;
    victim->data = static_cast<uint8_t*>(allocLarge(victim->capacity));
    if (victim->data == nullptr) {
      victim->capacity = 0;
      victim->block = -1;
      return nullptr;
    }
  }
  if (!inflateRaw(base_ + dataOffset_ + offset, packed, victim->data, raw)) {
    victim->block = -1;
    return nullptr;
  }
  victim->block = static_cast<int32_t>(index);
  victim->usedAt = clock_;
  victim->size = raw;
  *size = raw;
  return victim->data;
}

}  // namespace talk
