// src/talk/ の読み上げ前処理を PC で動かすテスト用ドライバ。
// 1行1文を読み、VOICEVOX 風の記法（句 "/"、間 "、"、アクセント "'"、無声化 "_"）で出力する。
// --units を付けると、本体が再生するモーラの並びを出す（H:カ 高い音 / L:カ 低い音 / D:ス 無声 /
// P:180 間 ms。長音「ー」は前の母音に置き換え済み）。--units は旧方式の比較用。
// --wav はファームと共通の SpeechSynth で音程・長さ・長音を処理する。
//
//   talk_test <ja.dic> <en.dic> [--units] < sentences.txt

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "talk/EnReader.h"
#include "talk/JaDict.h"
#include "talk/TextReader.h"
#include "talk/SpeechSynth.h"
#include "speech_synth_checks.h"

static std::vector<uint8_t> readFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

static uint32_t u32(const uint8_t* p) {
  return p[0] | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

static int16_t decodeMulaw(uint8_t b) {
  b = ~b;
  const int value = (((b & 15) * 8 + 0x84) << ((b >> 4) & 7)) - 0x84;
  return (b & 0x80) ? -value : value;
}

static bool renderWav(const talk::Utterance& utterance, const char* packPath,
                      const char* wavPath) {
  const auto pack = readFile(packPath);
  if (pack.size() < 20 || std::string(pack.begin(), pack.begin() + 4) != "SCVP" ||
      pack[6] != 1) return false;
  const uint32_t rate = u32(pack.data() + 8);
  const uint32_t count = u32(pack.data() + 12);
  const uint32_t dataStart = 20 + u32(pack.data() + 16);
  if (rate < 4000 || rate > 48000 || dataStart > pack.size()) return false;
  std::map<std::string, std::pair<uint32_t, uint32_t>> clips;
  size_t pos = 20;
  for (uint32_t i = 0; i < count; ++i) {
    if (pos >= dataStart) return false;
    const size_t length = pack[pos++];
    if (pos + length + 12 > dataStart) return false;
    const std::string key(reinterpret_cast<const char*>(pack.data() + pos), length);
    pos += length;
    const uint32_t offset = u32(pack.data() + pos);
    const uint32_t samples = u32(pack.data() + pos + 4);
    if (uint64_t(dataStart) + offset + samples > pack.size()) return false;
    clips[key] = {dataStart + offset, samples};
    pos += 12;
  }
  talk::MoraRenderer renderer;
  std::vector<talk::SpeechUnit> units;
  talk::planSpeech(utterance, units);
  std::vector<int16_t> samples;
  bool joinable = false;
  for (const auto& unit : units) {
    if (unit.pauseMs) {
      samples.insert(samples.end(), rate * unit.pauseMs / 1000, 0);
      joinable = false;
      continue;
    }
    std::string key(1, unit.devoiced ? '\3' : unit.prosody.high ? '\2' : '\1');
    key += unit.kana;
    auto clip = clips.find(key);
    if (clip == clips.end() && unit.devoiced) {
      key[0] = unit.prosody.high ? '\2' : '\1';
      clip = clips.find(key);
    }
    if (clip == clips.end()) {
      std::fprintf(stderr, "missing mora: %s\n", unit.kana.c_str());
      return false;
    }
    const size_t n = clip->second.second;
    int16_t* input = renderer.input(n);
    if (!input) return false;
    for (size_t i = 0; i < n; ++i) input[i] = decodeMulaw(pack[clip->second.first + i]);
    size_t outputCount = 0;
    auto prosody = unit.prosody;
    prosody.voiced = key[0] != '\3';
    const int16_t* pcm = renderer.render(n, rate, prosody, outputCount);
    if (!pcm) return false;
    const size_t overlap = std::min<size_t>(240, rate / 200);
    size_t offset = 0;
    if (joinable && unit.join && samples.size() >= overlap && outputCount > 2 * overlap) {
      const size_t start = samples.size() - overlap;
      for (size_t i = 0; i < overlap; ++i) {
        const int32_t w = (i + 1) * 256 / (overlap + 1);
        samples[start + i] = (samples[start + i] * (256 - w) + pcm[i] * w) / 256;
      }
      offset = overlap;
    }
    samples.insert(samples.end(), pcm + offset, pcm + outputCount);
    joinable = true;
  }
  std::ofstream wav(wavPath, std::ios::binary);
  auto write16 = [&](uint16_t n) { wav.put(n & 255); wav.put(n >> 8); };
  auto write32 = [&](uint32_t n) { write16(n & 65535); write16(n >> 16); };
  wav.write("RIFF", 4); write32(36 + samples.size() * 2);
  wav.write("WAVEfmt ", 8); write32(16); write16(1); write16(1);
  write32(rate); write32(rate * 2); write16(2); write16(16);
  wav.write("data", 4); write32(samples.size() * 2);
  for (int16_t v : samples) write16(static_cast<uint16_t>(v));
  std::fprintf(stderr, "shared mora renderer: %zu units, %.2fs\n", units.size(),
               static_cast<double>(samples.size()) / rate);
  return wav.good();
}

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--self-test") return speechSynthChecks();
  if (argc < 3) {
    std::fprintf(stderr, "usage: talk_test ja.dic en.dic < lines\n");
    return 2;
  }
  const std::vector<uint8_t> ja = readFile(argv[1]);
  const std::vector<uint8_t> en = readFile(argv[2]);
  talk::JaDict jaDict;
  talk::EnDict enDict;
  if (!jaDict.open(ja.data(), ja.size())) {
    std::fprintf(stderr, "ja.dic open failed\n");
    return 1;
  }
  if (!enDict.open(en.data(), en.size())) {
    std::fprintf(stderr, "en.dic open failed\n");
    return 1;
  }
  talk::EnReader enReader;
  enReader.setDict(&enDict);
  talk::TextReader reader(jaDict, enReader);
  const bool units = argc > 3 && std::string(argv[3]) == "--units";

  std::string line;
  double totalMs = 0;
  int lines = 0;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto t0 = std::chrono::steady_clock::now();
    talk::Utterance u;
    reader.read(line, u);
    const auto t1 = std::chrono::steady_clock::now();
    totalMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
    ++lines;
    if (argc == 6 && std::string(argv[3]) == "--wav") {
      return renderWav(u, argv[4], argv[5]) ? 0 : 1;
    }
    if (!units) {
      std::printf("%s\n", talk::toNotation(u).c_str());
      continue;
    }
    // BuiltinVoice::planSpeech と同じ規則でモーラを並べる
    std::string out;
    for (const talk::AccentPhrase& ph : u) {
      char vowel = 0;
      for (size_t i = 0; i < ph.moras.size(); ++i) {
        const talk::Mora& m = ph.moras[i];
        if (m.kana == "ッ") {
          out += "P:70 ";
          continue;
        }
        const bool high = talk::moraIsHigh(ph, i) || (ph.question && i + 1 == ph.moras.size());
        std::string name = talk::playableMora(m.kana);
        if (name == "ー") {
          if (!vowel) continue;
          name = talk::vowelKana(vowel);
        } else if (const char v = talk::moraVowel(name)) {
          vowel = v;
        }
        out += std::string(m.devoiced ? "D:" : high ? "H:" : "L:") + name + " ";
      }
      if (ph.pauseAfterMs) out += "P:" + std::to_string(ph.pauseAfterMs) + " ";
    }
    std::printf("%s\n", out.c_str());
  }
  std::fprintf(stderr, "%d lines, %.2f ms/line\n", lines, lines ? totalMs / lines : 0.0);
  return 0;
}
