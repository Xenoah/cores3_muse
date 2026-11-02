#include "Numbers.h"

#include <cstring>

#include "Kana.h"

namespace talk {

namespace {

const char* const kDigitJa[10] = {"ゼロ", "イチ", "ニ", "サン", "ヨン",
                                  "ゴ",   "ロク", "ナナ", "ハチ", "キュー"};
// 1桁目の読みのアクセント型（イ'チ、ニ'、サン、ヨ'ン…）
const uint8_t kDigitAccent[10] = {1, 2, 1, 0, 1, 1, 2, 1, 2, 1};

void addPart(NumberReading& r, const std::string& part, uint32_t unit,
             uint8_t accent) {
  if (r.parts.empty()) r.accent = accent;
  r.parts.push_back(part);
  r.lastUnit = unit;
}

// 0〜9999 を読む
void readGroup(int value, NumberReading& r) {
  const int thousands = value / 1000;
  const int hundreds = value / 100 % 10;
  const int tens = value / 10 % 10;
  const int ones = value % 10;
  if (thousands > 0) {
    if (thousands == 3) {
      addPart(r, "サン", 3, 0);
      addPart(r, "ゼン", 1000, 0);
    } else if (thousands == 8) {
      addPart(r, "ハッ", 8, 2);
      addPart(r, "セン", 1000, 0);
    } else {
      if (thousands > 1) addPart(r, kDigitJa[thousands], thousands, kDigitAccent[thousands]);
      addPart(r, "セン", 1000, 1);
    }
  }
  if (hundreds > 0) {
    if (hundreds == 3) {
      addPart(r, "サン", 3, 0);
      addPart(r, "ビャク", 100, 0);
    } else if (hundreds == 6) {
      addPart(r, "ロッ", 6, 2);
      addPart(r, "ピャク", 100, 0);
    } else if (hundreds == 8) {
      addPart(r, "ハッ", 8, 2);
      addPart(r, "ピャク", 100, 0);
    } else {
      if (hundreds > 1) addPart(r, kDigitJa[hundreds], hundreds, kDigitAccent[hundreds]);
      addPart(r, "ヒャク", 100, 2);
    }
  }
  if (tens > 0) {
    if (tens > 1) addPart(r, kDigitJa[tens], tens, kDigitAccent[tens]);
    addPart(r, "ジュー", 10, 1);
  }
  if (ones > 0) addPart(r, kDigitJa[ones], ones, kDigitAccent[ones]);
}

bool startsWith(const std::string& s, const char* prefix) {
  return s.compare(0, strlen(prefix), prefix) == 0;
}

}  // namespace

std::string NumberReading::pron() const {
  std::string out;
  for (const std::string& p : parts) out += p;
  return out;
}

NumberReading readNumberJa(const std::string& digitsIn) {
  NumberReading r;
  std::string digits = digitsIn;
  std::string fraction;
  const size_t dot = digits.find('.');
  if (dot != std::string::npos) {
    fraction = digits.substr(dot + 1);
    digits = digits.substr(0, dot);
  }
  if (digits.empty()) digits = "0";

  const bool digitByDigit =
      (digits.size() > 1 && digits[0] == '0') || digits.size() > 16;
  if (digitByDigit) {
    for (char c : digits) addPart(r, kDigitJa[c - '0'], c - '0', kDigitAccent[c - '0']);
  } else {
    // 4桁ずつ（万・億・兆）
    static const char* const kUnits[4] = {"", "マン", "オク", "チョー"};
    // 最後の部品の目印（億・兆は値そのものではなく区別用）
    static const uint32_t kUnitValues[4] = {1, 10000, 100000000, 1000000000};
    const int groups = static_cast<int>((digits.size() + 3) / 4);
    bool any = false;
    for (int g = groups - 1; g >= 0; --g) {
      const size_t end = digits.size() - g * 4;
      const size_t start = end >= 4 ? end - 4 : 0;
      const int value = atoi(digits.substr(start, end - start).c_str());
      if (value == 0) continue;
      any = true;
      if (g == 3 && value == 1) {
        addPart(r, "イッ", 1, 2);  // 1兆 = イッチョウ
      } else {
        readGroup(value, r);
      }
      if (g > 0) addPart(r, kUnits[g], kUnitValues[g], 1);
    }
    if (!any) addPart(r, "ゼロ", 0, 1);
  }

  if (!fraction.empty()) {
    addPart(r, "テン", 0, 0);
    for (char c : fraction) {
      if (c >= '0' && c <= '9') addPart(r, kDigitJa[c - '0'], c - '0', 0);
    }
  }
  return r;
}

bool isKanjiNumeral(uint32_t c) {
  switch (c) {
    case U'〇': case U'零': case U'一': case U'二': case U'三': case U'四':
    case U'五': case U'六': case U'七': case U'八': case U'九': case U'十':
    case U'百': case U'千': case U'万': case U'億': case U'兆':
      return true;
    default:
      return false;
  }
}

std::string kanjiNumeralToDigits(const std::u32string& text) {
  // 「二十五」「三千四百」「一億二千万」も「二〇二五」も読めるようにする
  auto digitOf = [](uint32_t c) -> int {
    switch (c) {
      case U'〇': case U'零': return 0;
      case U'一': return 1; case U'二': return 2; case U'三': return 3;
      case U'四': return 4; case U'五': return 5; case U'六': return 6;
      case U'七': return 7; case U'八': return 8; case U'九': return 9;
      default: return -1;
    }
  };
  bool positional = true;  // 位取りの字（十百千万…）が無い → 1字ずつ並べた数
  for (uint32_t c : text) {
    if (digitOf(c) < 0) positional = false;
  }
  if (positional) {
    std::string out;
    for (uint32_t c : text) out += static_cast<char>('0' + digitOf(c));
    return out;
  }
  uint64_t total = 0;
  uint64_t section = 0;  // 万未満の部分
  int current = -1;      // 直前の数字
  for (uint32_t c : text) {
    const int d = digitOf(c);
    if (d >= 0) {
      current = d;
      continue;
    }
    uint64_t unit = 0;
    switch (c) {
      case U'十': unit = 10; break;
      case U'百': unit = 100; break;
      case U'千': unit = 1000; break;
      case U'万': unit = 10000; break;
      case U'億': unit = 100000000ULL; break;
      case U'兆': unit = 1000000000000ULL; break;
      default: return std::string();
    }
    if (unit < 10000) {
      section += (current < 0 ? 1 : current) * unit;
    } else {
      section += current < 0 ? 0 : current;
      if (section == 0) section = 1;
      total += section * unit;
      section = 0;
    }
    current = -1;
  }
  if (current >= 0) section += current;
  total += section;
  char buf[24];
  snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(total));
  return buf;
}

namespace {

// 助数詞の音の変化
enum CounterKind : uint8_t {
  kPlain,   // 変化なし
  kK,       // カ行など: 1・6・8・10・100 で促音（イッカイ、ロッコ）
  kS,       // サ・タ行: 1・8・10 で促音（イッサツ、ハッサイ）
  kH,       // ハ行: 1・6・8・10・100 で促音＋パ行、3 で濁音（イッポン、サンボン）
  kHp,      // ハ行（泊・歩）: 3・4 も パ行（サンパク）
  kPct,     // パーセント: 1・6・8・10・100 で促音
};

struct Counter {
  const char32_t* surface;
  const char* pron;
  CounterKind kind;
  const char* afterThree;  // 3 のあとの読み（階 → ガイ）、nullptr = そのまま
};

const Counter kCounters[] = {
    {U"分", "フン", kH, "プン"},    {U"本", "ホン", kH, "ボン"},
    {U"匹", "ヒキ", kH, "ビキ"},    {U"杯", "ハイ", kH, "バイ"},
    {U"泊", "ハク", kHp, "パク"},   {U"歩", "ホ", kHp, "ポ"},
    {U"回", "カイ", kK, nullptr},   {U"個", "コ", kK, nullptr},
    {U"件", "ケン", kK, nullptr},   {U"階", "カイ", kK, "ガイ"},
    {U"軒", "ケン", kK, "ゲン"},    {U"曲", "キョク", kK, nullptr},
    {U"か月", "カゲツ", kK, nullptr}, {U"ヶ月", "カゲツ", kK, nullptr},
    {U"カ月", "カゲツ", kK, nullptr}, {U"ヵ月", "カゲツ", kK, nullptr},
    {U"か所", "カショ", kK, nullptr}, {U"ヶ所", "カショ", kK, nullptr},
    {U"カ所", "カショ", kK, nullptr}, {U"箇所", "カショ", kK, nullptr},
    {U"冊", "サツ", kS, nullptr},   {U"歳", "サイ", kS, nullptr},
    {U"才", "サイ", kS, nullptr},   {U"着", "チャク", kS, nullptr},
    {U"点", "テン", kS, nullptr},   {U"層", "ソー", kS, nullptr},
    {U"週", "シュー", kS, nullptr}, {U"周", "シュー", kS, nullptr},
    {U"足", "ソク", kS, "ゾク"},    {U"通", "ツー", kS, nullptr},
    {U"種類", "シュルイ", kS, nullptr},
    {U"%", "パーセント", kPct, nullptr}, {U"％", "パーセント", kPct, nullptr},
    {U"度", "ド", kPlain, nullptr}, {U"℃", "ド", kPlain, nullptr},
    {U"番", "バン", kPlain, nullptr}, {U"倍", "バイ", kPlain, nullptr},
    {U"台", "ダイ", kPlain, nullptr}, {U"枚", "マイ", kPlain, nullptr},
    {U"秒", "ビョー", kPlain, nullptr}, {U"位", "イ", kPlain, nullptr},
    {U"号", "ゴー", kPlain, nullptr}, {U"割", "ワリ", kPlain, nullptr},
    {U"次元", "ジゲン", kPlain, nullptr}, {U"層目", "ソーメ", kS, nullptr},
    {U"番目", "バンメ", kPlain, nullptr}, {U"日目", "ニチメ", kPlain, nullptr},
    {U"時", "ジ", kPlain, nullptr}, {U"時間", "ジカン", kPlain, nullptr},
    {U"年", "ネン", kPlain, nullptr}, {U"円", "エン", kPlain, nullptr},
    {U"人", "ニン", kPlain, nullptr}, {U"日", "ニチ", kPlain, nullptr},
    {U"月", "ガツ", kPlain, nullptr},
};

// 最後の部品を促音にする（イチ → イッ など）。できなければ false
bool makeSokuon(NumberReading& n) {
  if (n.parts.empty()) return false;
  std::string& last = n.parts.back();
  switch (n.lastUnit) {
    case 1: if (last == "イチ") { last = "イッ"; return true; } break;
    case 6: if (last == "ロク") { last = "ロッ"; return true; } break;
    case 8: if (last == "ハチ") { last = "ハッ"; return true; } break;
    case 10: if (last == "ジュー") { last = "ジュッ"; return true; } break;
    case 100: if (last == "ヒャク" || last == "ビャク" || last == "ピャク") {
      last = last.substr(0, last.size() - 3) + "ッ";  // ク → ッ
      return true;
    } break;
    default: break;
  }
  return false;
}

// 「フン」→「プン」（ハ行を半濁音にする）
std::string toP(const char* pron) {
  std::string s = pron;
  static const char* const kFrom[] = {"ハ", "ヒ", "フ", "ヘ", "ホ"};
  static const char* const kTo[] = {"パ", "ピ", "プ", "ペ", "ポ"};
  for (int i = 0; i < 5; ++i) {
    if (startsWith(s, kFrom[i])) return kTo[i] + s.substr(3);
  }
  return s;
}

}  // namespace

bool applyCounter(const std::u32string& counter, NumberReading& number,
                  std::string& counterPron) {
  const Counter* found = nullptr;
  for (const Counter& c : kCounters) {
    if (counter == c.surface) {
      found = &c;
      break;
    }
  }
  if (found == nullptr) return false;
  counterPron = found->pron;
  const uint32_t unit = number.lastUnit;
  const bool isOnly = number.parts.size() == 1;

  // 特別な読み
  if (counter == U"人" && isOnly && unit == 1) {
    number.parts.back() = "ヒト";
    counterPron = "リ";
    return true;
  }
  if (counter == U"人" && isOnly && unit == 2) {
    number.parts.back() = "フタ";
    counterPron = "リ";
    return true;
  }
  if (counter == U"日" && number.parts.size() <= 2) {
    static const struct { uint32_t value; const char* pron; } kDays[] = {
        {2, "フツカ"}, {3, "ミッカ"}, {4, "ヨッカ"}, {5, "イツカ"}, {6, "ムイカ"},
        {7, "ナノカ"}, {8, "ヨーカ"}, {9, "ココノカ"}, {10, "トーカ"}, {20, "ハツカ"}};
    const std::string p = number.pron();
    uint32_t value = 0;
    if (p == "ジュー") value = 10;
    else if (p == "ニジュー") value = 20;
    else if (isOnly) value = unit;
    for (const auto& d : kDays) {
      if (d.value == value) {
        number.parts.assign(1, d.pron);
        counterPron.clear();
        return true;
      }
    }
  }
  if (unit == 4 && (counter == U"時" || counter == U"時間" || counter == U"年" ||
                    counter == U"円" || counter == U"人")) {
    number.parts.back() = "ヨ";
    return true;
  }
  if (unit == 4 && counter == U"月") {
    number.parts.back() = "シ";
    return true;
  }
  if (unit == 7 && (counter == U"時" || counter == U"月")) {
    number.parts.back() = "シチ";
    return true;
  }
  if (unit == 9 && (counter == U"時" || counter == U"時間" || counter == U"月")) {
    number.parts.back() = "ク";
    return true;
  }

  switch (found->kind) {
    case kK:
    case kS:
    case kPct:
      if (found->kind == kS && (unit == 6 || unit == 100)) break;  // ロクサツ・ヒャクサツ
      makeSokuon(number);
      break;
    case kH:
    case kHp:
      if (makeSokuon(number) || (unit == 4 && counter == U"分")) {  // ヨンプン
        counterPron = toP(found->pron);
      } else if (unit == 3 || (found->kind == kHp && unit == 4) ||
                 unit == 1000 || unit == 10000) {
        if (found->afterThree) counterPron = found->afterThree;
      }
      break;
    default:
      break;
  }
  if (unit == 3 && found->afterThree && found->kind != kH && found->kind != kHp) {
    counterPron = found->afterThree;
  }
  return true;
}

std::string readNumberEn(const std::string& digits) {
  static const char* const kOnes[20] = {
      "ゼロ", "ワン", "ツー", "スリー", "フォー", "ファイブ", "シックス", "セブン",
      "エイト", "ナイン", "テン", "イレブン", "トゥエルブ", "サーティーン",
      "フォーティーン", "フィフティーン", "シックスティーン", "セブンティーン",
      "エイティーン", "ナインティーン"};
  static const char* const kTens[10] = {"", "", "トゥエンティ", "サーティ", "フォーティ",
                                        "フィフティ", "シックスティ", "セブンティ",
                                        "エイティ", "ナインティ"};
  std::string out;
  if (digits.size() > 3 || (digits.size() > 1 && digits[0] == '0')) {
    for (char c : digits) {
      if (c >= '0' && c <= '9') out += kOnes[c - '0'];
    }
    return out;
  }
  int value = atoi(digits.c_str());
  if (value >= 100) {
    out += kOnes[value / 100];
    out += "ハンドレッド";
    value %= 100;
    if (value == 0) return out;
  }
  if (value < 20) {
    out += kOnes[value];
  } else {
    out += kTens[value / 10];
    if (value % 10) out += kOnes[value % 10];
  }
  return out;
}

}  // namespace talk
