#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Kana.h"

namespace talk {

struct MoraProsody {
  uint16_t pitchStart = 4096;  // Q12 multiplier relative to the stored clip
  uint16_t pitchEnd = 4096;
  uint16_t vowelMs = 82;
  uint8_t gainStart = 255;
  uint8_t gainEnd = 255;
  bool high = false;
  bool voiced = true;
  bool continuation = false;  // reuse the previous mora's vowel, not its consonant
};

struct SpeechUnit {
  std::string kana;
  uint16_t pauseMs = 0;
  bool devoiced = false;
  bool join = false;
  MoraProsody prosody;
};

// Shared by the firmware and PC audio preview; no Arduino dependencies.
void planSpeech(const Utterance& utterance, std::vector<SpeechUnit>& units);

// Pitch-synchronous overlap-add of the voiced vowel. Consonants stay intact.
// Buffers grow only when needed and live in PSRAM on the ESP32.
class MoraRenderer {
 public:
  ~MoraRenderer();
  MoraRenderer() = default;
  MoraRenderer(const MoraRenderer&) = delete;
  MoraRenderer& operator=(const MoraRenderer&) = delete;
  int16_t* input(size_t samples);
  const int16_t* render(size_t samples, uint32_t rate, const MoraProsody& prosody,
                        size_t& outputSamples);

 private:
  int16_t* input_ = nullptr;
  size_t inputCapacity_ = 0;
  void* work_ = nullptr;
  size_t capacity_ = 0;
};

}  // namespace talk
