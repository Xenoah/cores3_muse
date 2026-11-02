#pragma once

#include <cmath>
#include <cstdio>
#include <stdexcept>

#include "talk/SpeechSynth.h"

inline void requireSpeech(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

inline int speechSynthChecks() {
  try {
    for (uint32_t rate : {8000U, 12000U, 24000U, 48000U}) {
      talk::MoraRenderer renderer;
      const size_t consonant = rate * 25 / 1000;
      const size_t n = consonant + rate * 96 / 1000;
      int16_t* input = renderer.input(n);
      requireSpeech(input != nullptr, "input allocation");
      for (size_t i = 0; i < n; ++i) {
        input[i] = i < consonant ? static_cast<int16_t>(i % 23 * 50) :
            static_cast<int16_t>(6000 * std::sin(2 * 3.141592653589793 * 343.78 *
                                               (i - consonant) / rate));
      }
      talk::MoraProsody p;
      p.pitchStart = p.pitchEnd = 5120;  // +25%, independent of duration
      p.vowelMs = 150;
      size_t size = 0;
      const int16_t* output = renderer.render(n, rate, p, size);
      requireSpeech(output && size == consonant + rate * 150 / 1000, "vowel duration");
      for (size_t i = rate * 2 / 1000; i < consonant; ++i) {
        requireSpeech(output[i] == input[i], "consonant was time/pitch shifted");
      }
      // Measure output F0 by positive-going zero crossings, away from the join.
      const size_t begin = consonant + rate * 30 / 1000;
      const size_t end = size - rate * 10 / 1000;
      unsigned crossings = 0;
      double energy = 0;
      for (size_t i = begin + 1; i < end; ++i) {
        crossings += output[i - 1] <= 0 && output[i] > 0;
        energy += double(output[i]) * output[i];
        requireSpeech(std::abs(int(output[i])) < 10000, "overlap gain/clipping");
      }
      const double hz = double(crossings) * rate / (end - begin);
      requireSpeech(std::abs(hz - 343.78 * 1.25) < 30, "pitch did not follow target");
      requireSpeech(std::sqrt(energy / (end - begin)) > 2500, "voiced energy dropout");
      p.continuation = true;
      output = renderer.render(n, rate, p, size);
      requireSpeech(output && size == rate * 150 / 1000, "long vowel repeats consonant");
      p.voiced = false;
      p.vowelMs = 55;
      requireSpeech(renderer.render(n, rate, p, size) && size == rate * 55 / 1000,
                    "unvoiced duration");
      p.vowelMs = 0;
      requireSpeech(!renderer.render(n, rate, p, size) && size == 0, "invalid duration accepted");
      std::printf("PASS waveform %uHz (F0 %.1fHz)\n", rate, hz);
    }
    talk::Utterance utterance(1);
    utterance[0].moras = {{"コ", false}, {"ー", false}, {"ヒ", false}, {"ー", false}};
    std::vector<talk::SpeechUnit> units;
    talk::planSpeech(utterance, units);
    requireSpeech(units.size() == 4 && units[1].kana == "コ" &&
                  units[1].prosody.continuation && units[3].kana == "ヒ" &&
                  units[3].prosody.continuation, "long vowel source selection");
    utterance[0].pauseAfterMs = 180;
    utterance.push_back(utterance[0]);
    units.clear();
    talk::planSpeech(utterance, units);
    requireSpeech(units.size() == 10 && units[4].pauseMs == 180 && !units[5].join,
                  "crossfade crossed punctuation pause");
    std::puts("PASS long vowels, pauses and waveform bounds");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
