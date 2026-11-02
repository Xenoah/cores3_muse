#include "BuiltinVoice.h"

#include <LittleFS.h>
#include <M5Unified.h>

#include "talk/EnReader.h"
#include "talk/BlockStore.h"
#include "talk/JaDict.h"
#include "talk/Kana.h"
#include "talk/TextReader.h"

// ファームウェアに埋め込んだ読み辞書（platformio.ini の board_build.embed_files）
extern const uint8_t kJaDicStart[] asm("_binary_dict_ja_dic_start");
extern const uint8_t kJaDicEnd[] asm("_binary_dict_ja_dic_end");
extern const uint8_t kEnDicStart[] asm("_binary_dict_en_dic_start");
extern const uint8_t kEnDicEnd[] asm("_binary_dict_en_dic_end");

namespace {

constexpr char kPackPath[] = "/voice.pak";
constexpr uint8_t kCodecAdpcm = 0;
constexpr uint8_t kCodecMulaw = 1;

// 再生バッファ（TtsClient と同じく3面を回し、再生中・再生待ち・書き込み中に使い分ける）
constexpr size_t kBufferCount = 3;
constexpr size_t kBufferSamples = 1024;
int16_t (*pcmBuffers)[kBufferSamples] = nullptr;
uint8_t* rawBuffer = nullptr;
int16_t* decodeBuffer = nullptr;

// モーラどうしを重ねる長さ（12kHz で約 5ms）
constexpr size_t kMaxOverlapSamples = 240;  // 5ms at the maximum 48kHz pack rate
int16_t tailBuffer[kMaxOverlapSamples];

// 文の区切りの間（ms）
constexpr uint16_t kSentencePauseMs = 220;
constexpr uint16_t kCommaPauseMs = 110;
constexpr uint16_t kAfterSentenceClipPauseMs = 150;

// モーラの音のキー（make_voice_pack.py と同じ）
constexpr char kMoraLow = '\x01';
constexpr char kMoraHigh = '\x02';
constexpr char kMoraDevoiced = '\x03';

const int16_t kStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
    19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
const int8_t kIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8,
                                -1, -1, -1, -1, 2, 4, 6, 8};

int16_t mulawToLinear(uint8_t value) {
  value = ~value;
  const int sign = value & 0x80;
  const int exponent = (value >> 4) & 0x07;
  const int mantissa = value & 0x0F;
  const int sample = (((mantissa << 3) + 0x84) << exponent) - 0x84;
  return static_cast<int16_t>(sign ? -sample : sample);
}

size_t utf8Length(uint8_t lead) {
  if (lead >= 0xF0) return 4;
  if (lead >= 0xE0) return 3;
  if (lead >= 0xC0) return 2;
  return 1;
}

bool startsWith(const char* s, size_t remaining, const char* prefix) {
  const size_t len = strlen(prefix);
  return remaining >= len && memcmp(s, prefix, len) == 0;
}

// 区切り記号なら間の長さを返す（len に記号のバイト数）
uint16_t pauseFor(const char* s, size_t remaining, size_t* len) {
  static const struct {
    const char* mark;
    uint16_t ms;
  } kMarks[] = {
      {"。", kSentencePauseMs}, {"！", kSentencePauseMs}, {"？", kSentencePauseMs},
      {"…", kSentencePauseMs},  {"、", kCommaPauseMs},    {"，", kCommaPauseMs},
      {"!", kSentencePauseMs},  {"?", kSentencePauseMs},  {".", kSentencePauseMs},
      {",", kCommaPauseMs},     {"　", 60},               {" ", 60},
      {"\n", kSentencePauseMs},
  };
  for (const auto& m : kMarks) {
    if (startsWith(s, remaining, m.mark)) {
      *len = strlen(m.mark);
      return m.ms;
    }
  }
  *len = 0;
  return 0;
}

bool endsWithSentenceMark(const char* key, size_t len) {
  static const char* const kMarks[] = {"。", "！", "？", "…"};
  for (const char* mark : kMarks) {
    const size_t ml = strlen(mark);
    if (len >= ml && memcmp(key + len - ml, mark, ml) == 0) return true;
  }
  return false;
}

// 読み辞書と読み上げの前処理（辞書はフラッシュ上をそのまま読む）
talk::JaDict jaDict;
talk::EnDict enDict;
talk::EnReader enReader;
talk::TextReader* textReader = nullptr;

// 音として出さない文字（空白・句読点・括弧）か
bool isSilentChar(const char* s, size_t remaining) {
  static const char* const kSilent[] = {
      " ", "　", "\n", "\r", "\t", "、", "。", "，", "．", "！", "？", "!", "?", ",", ".",
      "…", "「", "」", "『", "』", "（", "）", "(", ")", "・", "ー", "〜", "~", "-", ":", "："};
  for (const char* m : kSilent) {
    if (startsWith(s, remaining, m)) return true;
  }
  return false;
}

}  // namespace

bool BuiltinVoice::begin() {
  ready_ = false;
  moraReady_ = false;
  if (!pcmBuffers) {
    // Speaker_Class reads these samples into its own I2S buffers. They do not
    // need DMA-capable internal RAM; keep 9KiB available for Wi-Fi/TLS instead.
    const size_t pcmBytes = kBufferCount * kBufferSamples * sizeof(int16_t);
    auto* memory = static_cast<uint8_t*>(talk::allocLarge(pcmBytes + kBufferSamples * 3));
    if (!memory) {
      lastError_ = "voice buffer allocation failed";
      return false;
    }
    pcmBuffers = reinterpret_cast<int16_t (*)[kBufferSamples]>(memory);
    rawBuffer = memory + pcmBytes;
    decodeBuffer = reinterpret_cast<int16_t*>(rawBuffer + kBufferSamples);
  }
  if (!LittleFS.begin(false)) {
    lastError_ = "LittleFS mount failed (run: pio run -t uploadfs)";
    return false;
  }
  file_ = LittleFS.open(kPackPath, "r");
  if (!file_) {
    lastError_ = "voice pack not found (run tools/make_voice_pack.py)";
    return false;
  }

  uint8_t header[20];
  if (file_.read(header, sizeof(header)) != sizeof(header) ||
      memcmp(header, "SCVP", 4) != 0) {
    lastError_ = "invalid voice pack header";
    return false;
  }
  const uint16_t version = header[4] | (header[5] << 8);
  codec_ = header[6];
  memcpy(&rate_, header + 8, 4);
  uint32_t count = 0;
  uint32_t indexBytes = 0;
  memcpy(&count, header + 12, 4);
  memcpy(&indexBytes, header + 16, 4);
  if (version != 1 || (codec_ != kCodecAdpcm && codec_ != kCodecMulaw) ||
      count == 0 || count > 20000 || rate_ < 4000 || rate_ > 48000) {
    lastError_ = "unsupported voice pack";
    return false;
  }

  index_ = static_cast<char*>(ps_malloc(indexBytes));
  clips_ = static_cast<Clip*>(ps_malloc(sizeof(Clip) * count));
  if (index_ == nullptr || clips_ == nullptr ||
      file_.read(reinterpret_cast<uint8_t*>(index_), indexBytes) != indexBytes) {
    lastError_ = "voice pack index read failed";
    return false;
  }

  // 索引: [u8 keyLen][key][u32 offset][u32 samples][i16 predictor][u8 step][u8 0]
  uint32_t pos = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (pos + 1 > indexBytes) break;
    Clip& c = clips_[i];
    c.keyLen = static_cast<uint8_t>(index_[pos]);
    c.keyOffset = pos + 1;
    pos += 1 + c.keyLen;
    if (pos + 12 > indexBytes) {
      lastError_ = "voice pack index truncated";
      return false;
    }
    memcpy(&c.dataOffset, index_ + pos, 4);
    memcpy(&c.samples, index_ + pos + 4, 4);
    memcpy(&c.predictor, index_ + pos + 8, 2);
    c.stepIndex = static_cast<uint8_t>(index_[pos + 10]);
    pos += 12;
  }
  count_ = static_cast<uint16_t>(count);
  dataStart_ = sizeof(header) + indexBytes;

  // キーはバイト順に並んでいるので、先頭バイトごとの範囲を作っておく
  uint16_t c = 0;
  for (int b = 0; b < 256; ++b) {
    bucket_[b] = c;
    while (c < count_ && static_cast<uint8_t>(key(clips_[c])[0]) == b) ++c;
  }
  bucket_[256] = count_;

  ready_ = true;
  lastError_ = "";

  // モーラの音（新しいボイスパック）と読み辞書がそろえば、どんな文章でも読める
  const char kProbe[] = {kMoraHigh, '\xE3', '\x82', '\xA2', 0};  // "\x02ア"
  const bool haveMorae = findKey(kProbe, strlen(kProbe)) >= 0;
  const bool haveJa = jaDict.open(kJaDicStart, kJaDicEnd - kJaDicStart);
  const bool haveEn = enDict.open(kEnDicStart, kEnDicEnd - kEnDicStart);
  if (haveEn) enReader.setDict(&enDict);
  if (haveMorae && haveJa) {
    if (textReader == nullptr) textReader = new talk::TextReader(jaDict, enReader);
    moraReady_ = true;
  }
  Serial.printf("[voice] builtin voice ready: %u clips, %s %luHz, free text %s\n",
                count_, codec_ == kCodecMulaw ? "mulaw" : "adpcm",
                static_cast<unsigned long>(rate_),
                moraReady_ ? "on" : (haveMorae ? "off (dictionary)" : "off (old voice pack)"));
  return true;
}

void BuiltinVoice::setCallbacks(LipSyncCallback lipSync,
                                ServiceCallback service) {
  lipSync_ = lipSync;
  service_ = service;
}

int BuiltinVoice::findKey(const char* text, size_t len) const {
  int lo = 0;
  int hi = static_cast<int>(count_) - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    const Clip& c = clips_[mid];
    const int cmp = memcmp(key(c), text, min<size_t>(c.keyLen, len));
    if (cmp == 0 && c.keyLen == len) return mid;
    if (cmp < 0 || (cmp == 0 && c.keyLen < len)) {
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return -1;
}

int BuiltinVoice::longestPlainKeyAt(const char* text, size_t remaining,
                                    size_t* matched) const {
  *matched = 0;
  if (remaining == 0) return -1;
  const uint8_t first = static_cast<uint8_t>(text[0]);
  if (first == '#' || first == '=' || first < 0x04) return -1;  // 数字・モーラ用のキーは直接一致させない
  int best = -1;
  for (uint16_t i = bucket_[first]; i < bucket_[first + 1]; ++i) {
    const Clip& c = clips_[i];
    if (c.keyLen <= remaining && c.keyLen > *matched &&
        memcmp(key(c), text, c.keyLen) == 0) {
      best = i;
      *matched = c.keyLen;
    }
  }
  return best;
}

void BuiltinVoice::addClip(std::vector<Step>& steps, int clip, bool join) const {
  if (clip < 0) return;
  steps.push_back({static_cast<int16_t>(clip), 0,
                   join && !steps.empty() && steps.back().clip >= 0});
}

void BuiltinVoice::addPause(std::vector<Step>& steps, uint16_t ms) const {
  if (steps.empty()) return;  // 先頭の間は不要
  if (steps.back().clip < 0) {
    steps.back().pauseMs = max(steps.back().pauseMs, ms);
    return;
  }
  steps.push_back({-1, ms, false});
}

// 数字列 [begin, end) を読む。直後の単位（分・時間・% など）は促音・連濁を含めて
// まとめて録音したクリップ（"=23分" など）を優先する。戻り値は次に読む位置。
size_t BuiltinVoice::planNumber(const char* s, size_t n, size_t begin,
                                size_t end, std::vector<Step>& steps) const {
  char key[24];
  if (end - begin > 4) {
    // 5桁以上は1桁ずつ読む（実況ではまず出てこない）
    for (size_t i = begin; i < end; ++i) {
      snprintf(key, sizeof(key), "#%c", s[i]);
      addClip(steps, findKey(key, strlen(key)));
    }
    return end;
  }

  int value = 0;
  for (size_t i = begin; i < end; ++i) value = value * 10 + (s[i] - '0');
  const int total = value;
  if (value >= 1000) {
    snprintf(key, sizeof(key), "#%d", value / 1000 * 1000);
    addClip(steps, findKey(key, strlen(key)));
    value %= 1000;
  }
  if (value >= 100) {
    snprintf(key, sizeof(key), "#%d", value / 100 * 100);
    addClip(steps, findKey(key, strlen(key)));
    value %= 100;
  }

  static const char* const kUnits[] = {"時間", "%", "分", "時", "層", "件"};
  for (const char* unit : kUnits) {
    if (!startsWith(s + end, n - end, unit)) continue;
    // 「層目だよ」のように単位で始まる長い断片があればそちらに任せる
    size_t longer = 0;
    longestPlainKeyAt(s + end, n - end, &longer);
    if (longer > strlen(unit)) break;
    if (value > 0 || total == 0) {
      snprintf(key, sizeof(key), "=%d%s", value, unit);
      const int clip = findKey(key, strlen(key));
      if (clip >= 0) {
        addClip(steps, clip);
        return end + strlen(unit);
      }
    }
    break;
  }

  if (value > 0 || total == 0) {
    snprintf(key, sizeof(key), "#%d", value);
    addClip(steps, findKey(key, strlen(key)));
  }
  return end;
}

size_t BuiltinVoice::planPhrases(const std::string& text,
                                 std::vector<Step>& steps) const {
  const char* s = text.c_str();
  const size_t n = text.size();
  size_t skipped = 0;
  size_t i = 0;
  while (i < n) {
    const uint8_t c = static_cast<uint8_t>(s[i]);

    // 「ジョブ名」: モーラで読めれば読み、読めなければ「作品」と言い換える
    if (startsWith(s + i, n - i, "「")) {
      const char* close = strstr(s + i, "」");
      const size_t open = i + strlen("「");
      const size_t stop = close ? static_cast<size_t>(close - s) : n;
      if (moraReady_) {
        planSpeech(text.substr(open, stop - open), steps);
      } else {
        addClip(steps, findKey("作品", strlen("作品")));
      }
      i = close ? stop + strlen("」") : n;
      continue;
    }

    // 数字（先に「1層目が終わったよ！…」のような数字で始まる断片を探す）
    if (c >= '0' && c <= '9') {
      size_t end = i;
      while (end < n && s[end] >= '0' && s[end] <= '9') ++end;
      size_t matched = 0;
      const int clip = longestPlainKeyAt(s + i, n - i, &matched);
      if (clip >= 0 && matched > end - i) {
        addClip(steps, clip);
        i += matched;
        continue;
      }
      i = planNumber(s, n, i, end, steps);
      continue;
    }

    size_t markLen = 0;
    const uint16_t pause = pauseFor(s + i, n - i, &markLen);
    size_t matched = 0;
    const int clip = longestPlainKeyAt(s + i, n - i, &matched);
    if (clip >= 0) {
      if (pause > 0) addPause(steps, pause);
      addClip(steps, clip);
      if (endsWithSentenceMark(key(clips_[clip]), matched)) {
        addPause(steps, kAfterSentenceClipPauseMs);
      }
      i += matched;
      continue;
    }
    if (pause > 0) {
      addPause(steps, pause);
      i += markLen;
      continue;
    }
    if (!isSilentChar(s + i, n - i)) ++skipped;
    i += utf8Length(c);  // 読めない文字は飛ばす
  }
  return skipped;
}

void BuiltinVoice::planSpeech(const std::string& text, std::vector<Step>& steps) const {
  if (textReader == nullptr) return;
  talk::Utterance utterance;
  textReader->read(text, utterance);
  std::vector<talk::SpeechUnit> units;
  talk::planSpeech(utterance, units);
  for (const talk::SpeechUnit& unit : units) {
    if (unit.pauseMs) {
      addPause(steps, unit.pauseMs);
      continue;
    }
    auto append = [&](const std::string& kana, bool join) {
      int clip = -1;
      if (unit.devoiced) {
        const std::string key = std::string(1, kMoraDevoiced) + kana;
        clip = findKey(key.c_str(), key.size());
      }
      if (clip < 0) {
        const std::string key = std::string(1, unit.prosody.high ? kMoraHigh : kMoraLow) + kana;
        clip = findKey(key.c_str(), key.size());
      }
      if (clip < 0) return false;
      Step step = {static_cast<int16_t>(clip), 0, join};
      step.mora = true;
      step.prosody = unit.prosody;
      // A missing devoiced clip fell back to the voiced sample.
      step.prosody.voiced = key(clips_[clip])[0] != kMoraDevoiced;
      steps.push_back(step);
      return true;
    };
    if (!append(unit.kana, unit.join) && unit.kana.size() > 3) {
      append(unit.kana.substr(0, 3), unit.join);
      const char vowel = talk::moraVowel(unit.kana);
      if (vowel) append(talk::vowelKana(vowel), true);
    }
  }
}

void BuiltinVoice::plan(const String& text, std::vector<Step>& steps) {
  // 文ごとに、セリフのクリップで全部読めるならそれを、読めないならモーラで読む
  const std::string all(text.c_str(), text.length());
  size_t start = 0;
  while (start < all.size()) {
    size_t end = start;
    while (end < all.size()) {
      size_t len = 0;
      const uint16_t pause = pauseFor(all.c_str() + end, all.size() - end, &len);
      // 3.14 や v2.3 の「.」は文の終わりではない
      const bool decimalPoint = all[end] == '.' && end + 1 < all.size() &&
                                isdigit(static_cast<unsigned char>(all[end + 1]));
      if (pause >= kSentencePauseMs && len > 0 && !decimalPoint) {
        end += len;
        break;
      }
      end += utf8Length(static_cast<uint8_t>(all[end]));
    }
    const std::string sentence = all.substr(start, end - start);
    std::vector<Step> phraseSteps;
    const size_t skipped = planPhrases(sentence, phraseSteps);
    if (skipped == 0 || !moraReady_) {
      for (const Step& step : phraseSteps) {
        if (step.clip < 0) {
          addPause(steps, step.pauseMs);
        } else {
          steps.push_back(step);
        }
      }
    } else {
      planSpeech(sentence, steps);
      addPause(steps, kSentencePauseMs);
    }
    start = end;
  }
  while (!steps.empty() && steps.back().clip < 0) steps.pop_back();
}

bool BuiltinVoice::speak(const String& text) {
  if (!ready_) return false;
  std::vector<Step> steps;
  steps.reserve(64);
  plan(text, steps);
  bool hasClip = false;
  for (const Step& step : steps) hasClip |= step.clip >= 0;
  if (!hasClip) {
    lastError_ = "no speakable words";
    return false;
  }

  Serial.printf("[voice] builtin: %u steps\n", static_cast<unsigned>(steps.size()));
  talk::MoraRenderer renderer;
  bool ok = true;
  abort_ = false;
  bufferFill_ = 0;
  tailLen_ = 0;
  maxMoraUs_ = 0;
  for (size_t k = 0; k < steps.size(); ++k) {
    if (abort_) break;
    const Step& step = steps[k];
    if (step.clip < 0) {
      playSilence(step.pauseMs);
      continue;
    }
    // 次がつなげるモーラなら末尾を取っておいて重ねる
    const bool holdTail = k + 1 < steps.size() && steps[k + 1].join;
    if (!(step.mora ? playMora(step, holdTail, renderer)
                    : playClip(clips_[step.clip], step.join, holdTail))) {
      ok = false;
      break;
    }
  }
  if (abort_) {
    bufferFill_ = 0;
    tailLen_ = 0;
    M5.Speaker.stop(0);
  }
  flushTail();
  flushBuffer();
  while (M5.Speaker.isPlaying(0)) {
    if (service_) service_();
    delay(5);
  }
  if (lipSync_) lipSync_(0);
  Serial.printf("[voice] complete: ok=%u max_mora_us=%u\n", ok, maxMoraUs_);
  return ok;
}

void BuiltinVoice::submit(size_t sampleCount) {
  int16_t* samples = pcmBuffers[bufferIndex_];
  if (lipSync_) {
    // TtsClient と同じ基準で平均振幅を 0〜100 の口の開きにする
    uint32_t amplitude = 0;
    for (size_t i = 0; i < sampleCount; ++i) {
      amplitude += samples[i] < 0 ? -samples[i] : samples[i];
    }
    const int average = static_cast<int>(amplitude / max<size_t>(1, sampleCount));
    lipSync_(constrain((average - 180) * 100 / (5200 - 180), 0, 100));
  }
  M5.Speaker.playRaw(samples, sampleCount, rate_, false, 1, 0, false);
  bufferIndex_ = (bufferIndex_ + 1) % kBufferCount;
  if (service_) service_();
}

// 書き込み中のバッファへサンプルを足し、いっぱいになったら再生に回す
void BuiltinVoice::write(const int16_t* samples, size_t count) {
  while (count > 0) {
    const size_t n = min(count, kBufferSamples - bufferFill_);
    memcpy(pcmBuffers[bufferIndex_] + bufferFill_, samples, n * sizeof(int16_t));
    bufferFill_ += n;
    samples += n;
    count -= n;
    if (bufferFill_ == kBufferSamples) {
      submit(kBufferSamples);
      bufferFill_ = 0;
    }
  }
}

void BuiltinVoice::flushTail() {
  if (tailLen_ > 0) {
    write(tailBuffer, tailLen_);
    tailLen_ = 0;
  }
}

void BuiltinVoice::flushBuffer() {
  if (bufferFill_ > 0) {
    submit(bufferFill_);
    bufferFill_ = 0;
  }
}

bool BuiltinVoice::playMora(const Step& step, bool holdTail,
                            talk::MoraRenderer& renderer) {
  const Clip& clip = clips_[step.clip];
  // A corrupt pack must not turn a tiny mora into an unbounded allocation.
  if (clip.samples > rate_ / 2) return playClip(clip, step.join, holdTail);
  int16_t* input = renderer.input(clip.samples);
  if (!input) {
    lastError_ = "mora buffer allocation failed";
    return false;
  }
  if (!file_.seek(dataStart_ + clip.dataOffset)) {
    lastError_ = "voice pack seek failed";
    return false;
  }
  int predictor = clip.predictor;
  int stepIndex = clip.stepIndex;
  for (size_t pos = 0; pos < clip.samples;) {
    const size_t count = std::min<size_t>(kBufferSamples, clip.samples - pos);
    if (!decodeSamples(input + pos, count, predictor, stepIndex)) return false;
    pos += count;
  }
  size_t total = 0;
  const uint32_t started = micros();
  const int16_t* pcm = renderer.render(clip.samples, rate_, step.prosody, total);
  maxMoraUs_ = std::max(maxMoraUs_, static_cast<uint32_t>(micros() - started));
  if (!pcm) {
    lastError_ = "mora synthesis failed";
    return false;
  }
  if (!step.join) flushTail();
  size_t offset = 0;
  if (step.join && tailLen_) {
    offset = std::min(tailLen_, total);
    for (size_t i = 0; i < offset; ++i) {
      const int32_t w = (i + 1) * 256 / (offset + 1);
      tailBuffer[i] = (tailBuffer[i] * (256 - w) + pcm[i] * w) / 256;
    }
    write(tailBuffer, offset);
    tailLen_ = 0;
  }
  const size_t overlap = std::min<size_t>(kMaxOverlapSamples, rate_ / 200);
  const size_t hold = holdTail && total - offset > overlap ? overlap : 0;
  write(pcm + offset, total - offset - hold);
  if (hold) {
    memcpy(tailBuffer, pcm + total - hold, hold * sizeof(int16_t));
    tailLen_ = hold;
  }
  return true;
}

bool BuiltinVoice::decodeSamples(int16_t* out, size_t count, int& predictor, int& stepIndex) {
  if (codec_ == kCodecMulaw) {
    if (file_.read(rawBuffer, count) != count) {
      lastError_ = "voice pack read failed";
      return false;
    }
    for (size_t i = 0; i < count; ++i) out[i] = mulawToLinear(rawBuffer[i]);
  } else {
    // count は偶数（最後の端数だけ奇数になり得る）
    const size_t bytes = (count + 1) / 2;
    if (file_.read(rawBuffer, bytes) != bytes) {
      lastError_ = "voice pack read failed";
      return false;
    }
    for (size_t i = 0; i < count; ++i) {
      const uint8_t code =
          (i % 2 == 0) ? (rawBuffer[i / 2] & 0x0F) : (rawBuffer[i / 2] >> 4);
      const int step = kStepTable[stepIndex];
      int diff = step >> 3;
      if (code & 4) diff += step;
      if (code & 2) diff += step >> 1;
      if (code & 1) diff += step >> 2;
      predictor += (code & 8) ? -diff : diff;
      predictor = constrain(predictor, -32768, 32767);
      stepIndex = constrain(stepIndex + kIndexTable[code], 0, 88);
      out[i] = static_cast<int16_t>(predictor);
    }
  }
  return true;
}

bool BuiltinVoice::playClip(const Clip& clip, bool join, bool holdTail) {
  if (!file_.seek(dataStart_ + clip.dataOffset)) {
    lastError_ = "voice pack seek failed";
    return false;
  }
  if (!join) flushTail();
  const size_t total = clip.samples;
  const size_t overlap = std::min<size_t>(kMaxOverlapSamples, rate_ / 200);
  const size_t hold = holdTail && total > overlap * 2 ? overlap : 0;
  size_t done = 0;
  int predictor = clip.predictor;
  int stepIndex = clip.stepIndex;
  while (done < total) {
    const size_t count = min<size_t>(total - done, kBufferSamples);
    int16_t* out = decodeBuffer;
    if (!decodeSamples(out, count, predictor, stepIndex)) return false;

    // 前のモーラの末尾と重ねる（線形のクロスフェード）
    if (done == 0 && join && tailLen_ > 0) {
      const size_t n = min(tailLen_, count);
      for (size_t i = 0; i < n; ++i) {
        const int32_t w = static_cast<int32_t>((i + 1) * 256 / (n + 1));
        out[i] = static_cast<int16_t>((tailBuffer[i] * (256 - w) + out[i] * w) / 256);
      }
      tailLen_ = 0;
    }
    // 最後の数 ms（[total - hold, total)）は次のモーラと重ねるために取っておく
    size_t end = count;
    if (hold > 0 && done + count > total - hold) {
      const size_t holdStart = total - hold;
      const size_t local = holdStart > done ? holdStart - done : 0;  // このかたまりの中の位置
      const size_t offset = done + local - holdStart;                 // tailBuffer の中の位置
      memcpy(tailBuffer + offset, out + local, (count - local) * sizeof(int16_t));
      tailLen_ = offset + (count - local);
      end = local;
    }
    write(out, end);
    done += count;
  }
  return true;
}

void BuiltinVoice::playSilence(uint16_t ms) {
  flushTail();
  static const int16_t kZeros[256] = {};
  uint32_t remaining = rate_ * ms / 1000;
  while (remaining > 0) {
    const size_t n = min<uint32_t>(remaining, 256);
    write(kZeros, n);
    remaining -= n;
  }
}
