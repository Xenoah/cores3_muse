#include "SpeechSynth.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "BlockStore.h"

namespace talk {
namespace {

uint16_t pitchRatio(float logPitch, bool high) {
  // Source pitches in make_voice_pack.py (natural log Hz).
  const float ratio = std::exp(logPitch - (high ? 6.08f : 5.84f));
  return static_cast<uint16_t>(std::max(0.65f, std::min(1.4f, ratio)) * 4096);
}

int16_t sample(float value) {
  return static_cast<int16_t>(std::max(-32768.0f, std::min(32767.0f, value)));
}

}  // namespace

void planSpeech(const Utterance& utterance, std::vector<SpeechUnit>& units) {
  bool join = false;
  float previousPitch = 5.82f;
  unsigned phraseInBreath = 0;
  for (const AccentPhrase& phrase : utterance) {
    char previousVowel = 0;
    std::string previousKana;
    const float declination = std::min(0.10f, phraseInBreath * 0.025f);
    for (size_t i = 0; i < phrase.moras.size(); ++i) {
      const Mora& mora = phrase.moras[i];
      std::string kana = playableMora(mora.kana);
      if (kana == "ッ") {
        SpeechUnit pause;
        pause.pauseMs = 75;
        units.push_back(pause);
        join = false;
        previousVowel = 0;
        continue;
      }
      SpeechUnit unit;
      const bool longVowel = kana == "ー";
      const char vowel = longVowel ? previousVowel : moraVowel(kana);
      if (longVowel && !previousVowel) continue;
      // コー and コオ both sustain the original vowel, without a second onset.
      unit.prosody.continuation = !previousKana.empty() && previousVowel &&
          (longVowel || (kana == vowelKana(previousVowel) && !mora.devoiced));
      unit.kana = unit.prosody.continuation ? previousKana : kana;
      unit.devoiced = mora.devoiced;
      unit.join = join;
      unit.prosody.high = moraIsHigh(phrase, i);
      unit.prosody.voiced = !mora.devoiced;
      const bool last = i + 1 == phrase.moras.size();
      const bool breathEnd = last && (phrase.pauseAfterMs || &phrase == &utterance.back());
      float target = (unit.prosody.high ? 6.04f : 5.82f) - declination;
      if (breathEnd) target += phrase.question ? 0.14f : -0.075f;
      // A brief glide at the beginning of the vowel replaces abrupt pitch steps.
      unit.prosody.pitchStart = pitchRatio(join ? previousPitch : target - 0.025f,
                                           unit.prosody.high);
      unit.prosody.pitchEnd = pitchRatio(target, unit.prosody.high);
      unit.prosody.vowelMs = mora.devoiced ? 55 : breathEnd ? 105 : last ? 90 : 80;
      if (unit.prosody.continuation) unit.prosody.vowelMs = breathEnd ? 100 : 80;
      unit.prosody.gainStart = join ? 255 : 235;
      unit.prosody.gainEnd = breathEnd && !phrase.question ? 210 : 255;
      units.push_back(unit);
      previousPitch = target;
      previousVowel = mora.devoiced ? 0 : vowel;
      previousKana = unit.kana;
      join = true;
    }
    if (phrase.pauseAfterMs) {
      SpeechUnit pause;
      pause.pauseMs = phrase.pauseAfterMs;
      units.push_back(pause);
      join = false;
      phraseInBreath = 0;
    } else {
      ++phraseInBreath;
    }
  }
}

MoraRenderer::~MoraRenderer() {
  freeLarge(input_);
  freeLarge(work_);
}

int16_t* MoraRenderer::input(size_t samples) {
  if (samples > inputCapacity_) {
    auto* next = static_cast<int16_t*>(allocLarge(samples * sizeof(int16_t)));
    if (!next) return nullptr;
    freeLarge(input_);
    input_ = next;
    inputCapacity_ = samples;
  }
  return input_;
}

const int16_t* MoraRenderer::render(size_t samples, uint32_t rate,
                                    const MoraProsody& p, size_t& outputSamples) {
  outputSamples = 0;
  if (!input_ || samples > inputCapacity_ || samples < 8 || rate < 4000 ||
      rate > 48000 || p.vowelMs < 20 || p.vowelMs > 200) return nullptr;
  // The v1 voice pack stores 9 vowel frames at 93.75fps = 96ms.
  const size_t sourceVowel = std::min(samples, static_cast<size_t>(rate * 96 / 1000));
  const size_t vowelStart = samples - sourceVowel;
  const size_t prefix = p.continuation ? 0 : vowelStart;
  const size_t vowelLength = rate * p.vowelMs / 1000;
  const size_t total = prefix + vowelLength;
  if (total > capacity_) {
    const size_t capacity = (total + 127) & ~size_t(127);
    void* next = allocLarge(capacity * (2 * sizeof(float) + sizeof(int16_t)));
    if (!next) return nullptr;
    freeLarge(work_);
    work_ = next;
    capacity_ = capacity;
  }
  auto* sum = static_cast<float*>(work_);
  auto* weight = sum + capacity_;
  auto* out = reinterpret_cast<int16_t*>(weight + capacity_);
  std::fill(sum, sum + vowelLength, 0.0f);
  std::fill(weight, weight + vowelLength, 0.0f);
  if (prefix) std::memcpy(out, input_, prefix * sizeof(int16_t));

  const int expected = std::max(4, static_cast<int>(rate / (p.high ? 437.03f : 343.78f)));
  int period = expected;
  // Find the local pitch period, close to the known source pitch. Fixed source
  // F0 is only a hint: peak alignment must follow the actual waveform.
  if (p.voiced && sourceVowel > static_cast<size_t>(expected * 6)) {
    const int start = static_cast<int>(vowelStart + sourceVowel / 4);
    const int length = static_cast<int>(sourceVowel / 3);
    double best = std::numeric_limits<double>::max();
    for (int lag = expected * 4 / 5; lag <= expected * 6 / 5; ++lag) {
      double error = 0;
      for (int j = 0; j < length; ++j) {
        const double d = input_[start + j] - input_[start + j + lag];
        error += d * d;
      }
      if (error < best) { best = error; period = lag; }
    }
    int marks[128];
    int count = 0;
    int center = static_cast<int>(vowelStart) + period;
    while (center < static_cast<int>(samples) - period && count < 128) {
      int peak = center - period / 5;
      const int end = std::min(center + period / 5, static_cast<int>(samples) - period);
      for (int j = peak + 1; j <= end; ++j) {
        if (input_[j] > input_[peak]) peak = j;
      }
      marks[count++] = peak;
      center = peak + period;
    }
    if (count >= 2) {
      const float from = p.pitchStart / 4096.0f;
      const float to = p.pitchEnd / 4096.0f;
      float destination = 0;
      while (destination < vowelLength + period) {
        const float position = std::min(1.0f, destination / vowelLength);
        const int index = std::min(count - 1, static_cast<int>(position * (count - 1) + 0.5f));
        const int mark = marks[index];
        const int at = static_cast<int>(destination + 0.5f);
        for (int j = -period; j < period; ++j) {
          const int dest = at + j;
          if (dest < 0 || dest >= static_cast<int>(vowelLength)) continue;
          const float w = 0.5f + 0.5f * std::cos(3.14159265f * j / period);
          sum[dest] += input_[mark + j] * w;
          weight[dest] += w;
        }
        const float glide = std::min(1.0f, destination / (rate * 0.035f));
        const float pitch = std::max(0.65f, std::min(1.4f, from + (to - from) * glide));
        destination += period / pitch;
      }
    }
  }
  for (size_t i = 0; i < vowelLength; ++i) {
    // Unvoiced or too short: preserve the noise envelope by interpolation.
    float value;
    if (weight[i] > 0.0001f) value = sum[i] / weight[i];
    else {
      const float pos = static_cast<float>(i) * (sourceVowel - 1) / std::max<size_t>(1, vowelLength - 1);
      const size_t a = static_cast<size_t>(pos);
      const size_t b = std::min(a + 1, sourceVowel - 1);
      value = input_[vowelStart + a] + (input_[vowelStart + b] - input_[vowelStart + a]) * (pos - a);
    }
    const float fraction = static_cast<float>(i) / std::max<size_t>(1, vowelLength - 1);
    const float gain = (p.gainStart + (p.gainEnd - p.gainStart) * fraction) / 255.0f;
    out[prefix + i] = sample(value * gain);
  }
  // Blend the first two pitch periods into the preserved consonant/vowel onset.
  if (prefix && p.voiced) {
    const size_t blend = std::min<size_t>(period * 2, vowelLength);
    for (size_t i = 0; i < blend; ++i) {
      const float w = static_cast<float>(i + 1) / (blend + 1);
      out[prefix + i] = sample(input_[vowelStart + i] * (1 - w) + out[prefix + i] * w);
    }
  }
  const size_t fade = std::min(total / 2, static_cast<size_t>(rate * 15 / 10000));
  for (size_t i = 0; i < fade; ++i) {
    const float w = static_cast<float>(i + 1) / (fade + 1);
    out[i] = sample(out[i] * w);
    out[total - 1 - i] = sample(out[total - 1 - i] * w);
  }
  outputSamples = total;
  return out;
}

}  // namespace talk
