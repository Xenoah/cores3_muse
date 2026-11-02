#include "TextReader.h"

#include "Numbers.h"

namespace talk {

namespace {

constexpr uint16_t kSentencePauseMs = 350;
constexpr uint16_t kCommaPauseMs = 180;
constexpr uint16_t kShortPauseMs = 60;

bool isJapanese(uint32_t c) {
  return isHiragana(c) || isKatakana(c) || isKanji(c) || c == 0x30FD || c == 0x30FE ||
         c == 0x309D || c == 0x309E;
}
bool isLatin(uint32_t c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool isDigit(uint32_t c) { return c >= '0' && c <= '9'; }

// 半角カナ（U+FF66〜U+FF9D）→ 全角
const char32_t kHalfKana[] =
    U"ヲァィゥェォャュョッーアイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨラリルレロワン";

uint32_t voiced(uint32_t c, bool semi) {
  // カ〜ト・ハ〜ホ に濁点、ハ〜ホ に半濁点、ウ に濁点（ヴ）
  if (c == U'ウ' && !semi) return U'ヴ';
  if (!semi && ((c >= U'カ' && c <= U'ト' && (c - U'カ') % 2 == 0) ||
                (c >= U'ハ' && c <= U'ホ' && (c - U'ハ') % 3 == 0))) {
    return c + 1;
  }
  if (semi && c >= U'ハ' && c <= U'ホ' && (c - U'ハ') % 3 == 0) return c + 2;
  return c;
}

// 記号の読み（数のあとなら助数詞として applyCounter が扱うものもある）
const char* symbolReading(uint32_t c) {
  switch (c) {
    case '%': return "パーセント";
    case U'℃': return "ド";
    case U'°': return "ド";
    case '&': return "アンド";
    case '+': return "プラス";
    case '=': return "イコール";
    case '@': return "アット";
    case '#': return "シャープ";
    case U'×': return "カケル";
    case U'÷': return "ワル";
    case U'♪': return nullptr;
    default: return nullptr;
  }
}

}  // namespace

std::u32string normalizeText(const std::u32string& text) {
  std::u32string out;
  out.reserve(text.size());
  for (uint32_t c : text) {
    if (c >= 0xFF01 && c <= 0xFF5E) {
      c = c - 0xFF01 + 0x21;  // 全角英数・記号
    } else if (c == 0x3000) {
      c = ' ';
    } else if (c >= 0xFF66 && c <= 0xFF9D) {
      c = kHalfKana[c - 0xFF66];
    } else if ((c == 0xFF9E || c == 0x3099 || c == 0x309B) && !out.empty()) {
      out.back() = voiced(out.back(), false);
      continue;
    } else if ((c == 0xFF9F || c == 0x309A || c == 0x309C) && !out.empty()) {
      out.back() = voiced(out.back(), true);
      continue;
    } else if (c == 0x2212 || c == 0x2010 || c == 0x2011 || c == 0x2013 || c == 0x2014) {
      c = '-';
    } else if (c == 0xFF64) {
      c = U'、';
    } else if (c == 0xFF61) {
      c = U'。';
    } else if (c == 0x2019 || c == 0x2018) {
      c = '\'';
    }
    out += c;
  }
  return out;
}

void TextReader::flush(std::vector<JaToken>& tokens, Utterance& utterance,
                       uint16_t pauseMs, bool question) {
  const size_t before = utterance.size();
  jaReader_.buildPhrases(tokens, utterance);
  tokens.clear();
  if (utterance.size() > before) {
    utterance.back().pauseAfterMs = pauseMs;
    utterance.back().question = question;
  } else if (!utterance.empty() && pauseMs > utterance.back().pauseAfterMs) {
    utterance.back().pauseAfterMs = pauseMs;
    utterance.back().question |= question;
  }
}

void TextReader::addJapanese(const std::u32string& run,
                             std::vector<JaToken>& tokens) {
  JaReader::Context context = JaReader::Context::Start;
  if (!tokens.empty() && tokens.back().number) {
    context = JaReader::Context::Number;
  } else if (!tokens.empty() && tokens.back().latin) {
    context = JaReader::Context::Noun;
  }
  std::vector<JaToken> words;
  jaReader_.tokenize(run, context, words);

  // 漢数字の並び（名詞・数）は1つの数にまとめて読み直す（二十五 → ニジュウゴ）
  std::vector<JaToken> merged;
  for (size_t i = 0; i < words.size(); ++i) {
    std::u32string numeral;
    size_t j = i;
    while (j < words.size() && words[j].group == kGroupNumber) {
      bool allNumeral = !words[j].surface.empty();
      for (uint32_t c : words[j].surface) allNumeral &= isKanjiNumeral(c);
      if (!allNumeral) break;
      numeral += words[j].surface;
      ++j;
    }
    if (j > i) {
      const std::string digits = kanjiNumeralToDigits(numeral);
      if (!digits.empty()) {
        const NumberReading r = readNumberJa(digits);
        JaToken t;
        t.surface = numeral;
        t.pron = r.pron();
        t.accent = r.accent;
        t.group = kGroupNumber;
        t.number = true;
        t.digits = digits;
        merged.push_back(t);
        i = j - 1;
        continue;
      }
    }
    merged.push_back(words[i]);
  }

  // 数のあとの助数詞（3本 → サンボン、1人 → ヒトリ）
  auto applyAfter = [](JaToken& num, JaToken& counter) {
    if (!num.number || num.digits.empty()) return;
    NumberReading r = readNumberJa(num.digits);
    std::string pron;
    if (applyCounter(counter.surface, r, pron)) {
      num.pron = r.pron();
      counter.pron = pron;
      counter.group = kGroupSuffix;  // 数と同じ句にする
    }
  };
  if (!merged.empty() && !tokens.empty()) applyAfter(tokens.back(), merged.front());
  for (size_t i = 0; i + 1 < merged.size(); ++i) applyAfter(merged[i], merged[i + 1]);
  tokens.insert(tokens.end(), merged.begin(), merged.end());
}

void TextReader::read(const std::string& text, Utterance& utterance) {
  const std::u32string s = normalizeText(toU32(text));
  const size_t n = s.size();
  std::vector<JaToken> tokens;
  size_t i = 0;
  while (i < n) {
    const uint32_t c = s[i];

    // 日本語
    if (isJapanese(c)) {
      size_t j = i;
      while (j < n && isJapanese(s[j])) ++j;
      addJapanese(s.substr(i, j - i), tokens);
      i = j;
      continue;
    }

    // 英字（前後の数字を含む: 3D, P1S, PETG）
    const bool digitStartsWord = isDigit(c) && [&]() {
      size_t j = i;
      while (j < n && isDigit(s[j])) ++j;
      return j < n && isLatin(s[j]);
    }();
    if (isLatin(c) || digitStartsWord) {
      size_t j = i;
      while (j < n && (isLatin(s[j]) || isDigit(s[j]) ||
                       (s[j] == '\'' && j + 1 < n && isLatin(s[j + 1]) && j > i))) {
        ++j;
      }
      std::string word;
      for (size_t k = i; k < j; ++k) word += static_cast<char>(s[k]);
      std::vector<JaToken> words;
      en_.read(word, words);
      tokens.insert(tokens.end(), words.begin(), words.end());
      i = j;
      continue;
    }

    // 数（1,234 や 3.14 も）
    if (isDigit(c)) {
      std::string digits;
      size_t j = i;
      while (j < n) {
        if (isDigit(s[j])) {
          digits += static_cast<char>(s[j]);
        } else if (s[j] == ',' && j + 3 < n + 1 && j + 1 < n && isDigit(s[j + 1]) &&
                   !digits.empty()) {
          // 桁区切りのカンマ（後ろに3桁続くときだけ）
          size_t k = j + 1;
          while (k < n && isDigit(s[k])) ++k;
          if (k - (j + 1) != 3) break;
        } else if (s[j] == '.' && j + 1 < n && isDigit(s[j + 1]) &&
                   digits.find('.') == std::string::npos) {
          digits += '.';
        } else {
          break;
        }
        ++j;
      }
      // 1億3,000万・5千 のような漢字の位取りとの混在
      if (j < n && (s[j] == U'万' || s[j] == U'億' || s[j] == U'兆' || s[j] == U'千' ||
                    s[j] == U'百') && digits.find('.') == std::string::npos) {
        std::u32string mixed;
        size_t k = i;
        while (k < n) {
          if (isDigit(s[k])) {
            // 数字の並びを漢数字に置き換える（位取りの計算は kanjiNumeralToDigits に任せる）
            size_t e = k;
            std::string part;
            while (e < n && (isDigit(s[e]) || (s[e] == ',' && e + 1 < n && isDigit(s[e + 1])))) {
              if (isDigit(s[e])) part += static_cast<char>(s[e]);
              ++e;
            }
            const std::u32string numerals = U"〇一二三四五六七八九";
            // 千・百の前の数字は1桁、万・億・兆の前は4桁まで（それ以外は読み方を変えない）
            if (part.size() > 4) break;
            const unsigned long v = strtoul(part.c_str(), nullptr, 10);
            if (v >= 1000) mixed += numerals[v / 1000], mixed += U'千';
            if (v % 1000 >= 100) mixed += numerals[v / 100 % 10], mixed += U'百';
            if (v % 100 >= 10) mixed += numerals[v / 10 % 10], mixed += U'十';
            if (v % 10) mixed += numerals[v % 10];
            k = e;
            continue;
          }
          if (s[k] == U'万' || s[k] == U'億' || s[k] == U'兆' || s[k] == U'千' || s[k] == U'百') {
            mixed += s[k];
            ++k;
            continue;
          }
          break;
        }
        const std::string total = kanjiNumeralToDigits(mixed);
        if (!total.empty() && k > j) {
          digits = total;
          j = k;
        }
      }
      // マイナス
      if (i > 0 && s[i - 1] == '-' && (i < 2 || !isDigit(s[i - 2]))) {
        JaToken minus;
        minus.pron = "マイナス";
        minus.accent = 0;
        tokens.push_back(minus);
      }
      const NumberReading r = readNumberJa(digits);
      JaToken t;
      for (size_t k = i; k < j; ++k) t.surface += s[k];
      t.pron = r.pron();
      t.accent = r.accent;
      t.group = kGroupNumber;
      t.number = true;
      t.digits = digits;
      tokens.push_back(t);
      i = j;
      // 直後の % ℃ などの助数詞
      if (i < n && (s[i] == '%' || s[i] == U'℃' || s[i] == U'°')) {
        JaToken unit;
        unit.surface = std::u32string(1, s[i] == U'°' ? U'℃' : s[i]);
        unit.pron = symbolReading(s[i]);
        unit.group = kGroupSuffix;
        NumberReading nr = readNumberJa(digits);
        std::string pron;
        if (applyCounter(unit.surface, nr, pron)) {
          tokens.back().pron = nr.pron();
          unit.pron = pron;
        }
        tokens.push_back(unit);
        ++i;
      }
      continue;
    }

    // 句読点・区切り
    switch (c) {
      case U'。': case '.': case U'！': case '!': case U'？': case '?':
      case U'…': case '\n': case U'‥': {
        bool question = false;
        size_t j = i;
        while (j < n && (s[j] == U'。' || s[j] == '.' || s[j] == '!' || s[j] == '?' ||
                         s[j] == U'…' || s[j] == U'‥' || s[j] == '\n' || s[j] == U'！' ||
                         s[j] == U'？')) {
          question |= s[j] == '?' || s[j] == U'？';
          ++j;
        }
        flush(tokens, utterance, kSentencePauseMs, question);
        i = j;
        continue;
      }
      case U'、': case ',': case ';': case ':': case U'；': case U'：':
        flush(tokens, utterance, kCommaPauseMs, false);
        ++i;
        continue;
      case U'・': case '/': case '-': case '~': case U'〜':
        flush(tokens, utterance, kShortPauseMs, false);
        ++i;
        continue;
      case ' ': case '\t': case '\r':
      case U'「': case U'」': case U'『': case U'』': case '(': case ')':
      case U'（': case U'）': case '[': case ']': case U'【': case U'】':
      case '"': case U'“': case U'”': case '\'':
        flush(tokens, utterance, 0, false);  // 句を区切るだけ
        ++i;
        continue;
      default:
        break;
    }

    // 読める記号
    if (const char* reading = symbolReading(c)) {
      JaToken t;
      t.surface = std::u32string(1, c);
      t.pron = reading;
      t.accent = 0;
      tokens.push_back(t);
    }
    ++i;  // それ以外の記号・絵文字は読まない
  }
  flush(tokens, utterance, kSentencePauseMs, false);
  if (!utterance.empty()) utterance.back().pauseAfterMs = 0;  // 最後の間は不要
  markDevoicing(utterance);
}

}  // namespace talk
