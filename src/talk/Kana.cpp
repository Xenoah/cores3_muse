#include "Kana.h"

#include <cstring>

namespace talk {

uint32_t decodeUtf8(const std::string& s, size_t& pos) {
  const uint8_t c = static_cast<uint8_t>(s[pos]);
  if (c < 0x80) {
    ++pos;
    return c;
  }
  int len = 0;
  uint32_t cp = 0;
  if ((c & 0xE0) == 0xC0) {
    len = 2;
    cp = c & 0x1F;
  } else if ((c & 0xF0) == 0xE0) {
    len = 3;
    cp = c & 0x0F;
  } else if ((c & 0xF8) == 0xF0) {
    len = 4;
    cp = c & 0x07;
  } else {
    ++pos;
    return 0xFFFD;
  }
  if (pos + len > s.size()) {
    pos = s.size();
    return 0xFFFD;
  }
  for (int i = 1; i < len; ++i) {
    const uint8_t cc = static_cast<uint8_t>(s[pos + i]);
    if ((cc & 0xC0) != 0x80) {
      ++pos;
      return 0xFFFD;
    }
    cp = (cp << 6) | (cc & 0x3F);
  }
  pos += len;
  return cp;
}

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

std::u32string toU32(const std::string& s) {
  std::u32string out;
  size_t pos = 0;
  while (pos < s.size()) out += decodeUtf8(s, pos);
  return out;
}

std::string toUtf8(const std::u32string& s) {
  std::string out;
  for (uint32_t c : s) appendUtf8(out, c);
  return out;
}

bool isHiragana(uint32_t c) { return c >= 0x3041 && c <= 0x3096; }
bool isKatakana(uint32_t c) { return (c >= 0x30A1 && c <= 0x30FA) || c == 0x30FC; }
bool isKanji(uint32_t c) {
  return (c >= 0x3400 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) ||
         c == 0x3005 || c == 0x3006 || c == 0x30F6;
}
bool isKanaSmall(uint32_t c) {
  switch (c) {
    case 0x30A1: case 0x30A3: case 0x30A5: case 0x30A7: case 0x30A9:  // ァィゥェォ
    case 0x30E3: case 0x30E5: case 0x30E7: case 0x30EE:                // ャュョヮ
    case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049:
    case 0x3083: case 0x3085: case 0x3087: case 0x308E:
      return true;
    default:
      return false;
  }
}
uint32_t hiraToKata(uint32_t c) { return isHiragana(c) ? c + 0x60 : c; }

void splitMoras(const std::string& katakana, std::vector<Mora>& out) {
  size_t pos = 0;
  bool lastSpecial = true;  // 直前が ッ・ン・ー（小書き文字をつなげない）
  while (pos < katakana.size()) {
    const uint32_t c = decodeUtf8(katakana, pos);
    if (c == 0x2019) {  // ’ = 直前のモーラは無声化
      if (!out.empty()) out.back().devoiced = true;
      continue;
    }
    const uint32_t k = hiraToKata(c);
    if (isKanaSmall(k) && !out.empty() && !lastSpecial &&
        out.back().kana.size() <= 3) {
      appendUtf8(out.back().kana, k);
      continue;
    }
    Mora m;
    appendUtf8(m.kana, k);
    lastSpecial = k == 0x30C3 || k == 0x30F3 || k == 0x30FC;
    out.push_back(m);
  }
}

namespace {

// カタカナ（1文字）の母音
char vowelOfKana(uint32_t k) {
  static const char* const kRows[5] = {
      "アカサタナハマヤラワガザダバパァャヮ",
      "イキシチニヒミリギジヂビピィ",
      "ウクスツヌフムユルグズヅブプゥュヴ",
      "エケセテネヘメレゲゼデベペェ",
      "オコソトノホモヨロヲゴゾドボポォョ",
  };
  static const char kVowels[5] = {'a', 'i', 'u', 'e', 'o'};
  for (int r = 0; r < 5; ++r) {
    const std::string row = kRows[r];
    size_t pos = 0;
    while (pos < row.size()) {
      if (decodeUtf8(row, pos) == k) return kVowels[r];
    }
  }
  return 0;
}

}  // namespace

std::string playableMora(const std::string& kana) {
  static const struct {
    const char* from;
    const char* to;
  } kMap[] = {{"ヲ", "オ"}, {"ヂ", "ジ"}, {"ヅ", "ズ"}, {"ヰ", "イ"}, {"ヱ", "エ"},
              {"ヵ", "カ"}, {"ヶ", "ケ"}, {"ヮ", "ワ"}};
  std::string out = kana;
  for (const auto& m : kMap) {
    size_t pos;
    while ((pos = out.find(m.from)) != std::string::npos) {
      out.replace(pos, std::strlen(m.from), m.to);
    }
  }
  return out;
}

char moraVowel(const std::string& kana) {
  if (kana.empty()) return 0;
  // 最後の文字で決まる（キャ → ャ → a）
  size_t pos = 0;
  uint32_t last = 0;
  while (pos < kana.size()) last = decodeUtf8(kana, pos);
  if (last == 0x30C3 || last == 0x30F3 || last == 0x30FC) return 0;
  return vowelOfKana(last);
}

const char* vowelKana(char vowel) {
  switch (vowel) {
    case 'a': return "ア";
    case 'i': return "イ";
    case 'u': return "ウ";
    case 'e': return "エ";
    case 'o': return "オ";
    default:  return "";
  }
}

bool moraVoicelessConsonant(const std::string& kana) {
  if (kana.empty()) return false;
  size_t pos = 0;
  const uint32_t first = decodeUtf8(kana, pos);
  static const char kVoiceless[] =
      "カキクケコサシスセソタチツテトハヒフヘホパピプペポ";
  const std::string list = kVoiceless;
  size_t p = 0;
  while (p < list.size()) {
    if (decodeUtf8(list, p) == first) return true;
  }
  return false;
}

bool moraIsHigh(const AccentPhrase& phrase, size_t index) {
  const int acc = phrase.accent;
  if (acc == 1) return index == 0;
  if (acc == 0) return index > 0;
  return index > 0 && static_cast<int>(index) < acc;
}

std::string toNotation(const Utterance& utterance) {
  std::string out;
  for (size_t p = 0; p < utterance.size(); ++p) {
    const AccentPhrase& ph = utterance[p];
    std::string prevVowelKana;
    for (size_t i = 0; i < ph.moras.size(); ++i) {
      const Mora& m = ph.moras[i];
      if (m.devoiced) out += "_";
      if (m.kana == "ー" && !prevVowelKana.empty()) {
        out += prevVowelKana;
      } else {
        out += m.kana;
        const char v = moraVowel(m.kana);
        if (v) prevVowelKana = vowelKana(v);
      }
      if (ph.accent == static_cast<int>(i) + 1) out += "'";
    }
    if (ph.question) out += "？";
    if (p + 1 < utterance.size()) out += ph.pauseAfterMs > 0 ? "、" : "/";
  }
  return out;
}

}  // namespace talk
