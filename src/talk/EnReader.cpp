#include "EnReader.h"

#include <cctype>
#include <cstring>

#include "Numbers.h"

namespace talk {

namespace {

// 音素コード（make_dict.py の phone_code と同じ）
enum Vowel : uint8_t { AA, AE, AH, AO, AW, AY, EH, ER, EY, IH, IY, OW, OY, UH, UW };
enum Consonant : uint8_t {
  B, CH, D, DH, F, G, HH, JH, K, L, M, N, NG, P, R, S, SH, T, TH, V, W, Y, Z, ZH, kNoConsonant
};
constexpr uint8_t kConsonantBase = 45;

struct Phone {
  bool vowel;
  uint8_t id;      // Vowel または Consonant
  uint8_t stress;  // 母音の強勢 0〜2
};

Phone decodePhone(uint8_t code) {
  if (code < kConsonantBase) return {true, static_cast<uint8_t>(code / 3), static_cast<uint8_t>(code % 3)};
  return {false, static_cast<uint8_t>(code - kConsonantBase), 0};
}

// 子音 × 母音の段（a i u e o）のカタカナ
const char* const kRows[][5] = {
    /* B  */ {"バ", "ビ", "ブ", "ベ", "ボ"},
    /* CH */ {"チャ", "チ", "チュ", "チェ", "チョ"},
    /* D  */ {"ダ", "ディ", "ドゥ", "デ", "ド"},
    /* DH */ {"ザ", "ジ", "ズ", "ゼ", "ゾ"},
    /* F  */ {"ファ", "フィ", "フ", "フェ", "フォ"},
    /* G  */ {"ガ", "ギ", "グ", "ゲ", "ゴ"},
    /* HH */ {"ハ", "ヒ", "フ", "ヘ", "ホ"},
    /* JH */ {"ジャ", "ジ", "ジュ", "ジェ", "ジョ"},
    /* K  */ {"カ", "キ", "ク", "ケ", "コ"},
    /* L  */ {"ラ", "リ", "ル", "レ", "ロ"},
    /* M  */ {"マ", "ミ", "ム", "メ", "モ"},
    /* N  */ {"ナ", "ニ", "ヌ", "ネ", "ノ"},
    /* NG */ {"ガ", "ギ", "グ", "ゲ", "ゴ"},
    /* P  */ {"パ", "ピ", "プ", "ペ", "ポ"},
    /* R  */ {"ラ", "リ", "ル", "レ", "ロ"},
    /* S  */ {"サ", "シ", "ス", "セ", "ソ"},
    /* SH */ {"シャ", "シ", "シュ", "シェ", "ショ"},
    /* T  */ {"タ", "ティ", "トゥ", "テ", "ト"},
    /* TH */ {"サ", "シ", "ス", "セ", "ソ"},
    /* V  */ {"バ", "ビ", "ブ", "ベ", "ボ"},
    /* W  */ {"ワ", "ウィ", "ウ", "ウェ", "ウォ"},
    /* Y  */ {"ヤ", "イ", "ユ", "イェ", "ヨ"},
    /* Z  */ {"ザ", "ジ", "ズ", "ゼ", "ゾ"},
    /* ZH */ {"ジャ", "ジ", "ジュ", "ジェ", "ジョ"},
    /* -  */ {"ア", "イ", "ウ", "エ", "オ"},
};

// 拗音（キャ・ピュ など）: 子音の イ段 ＋ 小さい ャュョ
const char* const kYoonBase[] = {
    /* B */ "ビ", /* CH */ "チ", /* D */ "デ", /* DH */ "ジ", /* F */ "フ", /* G */ "ギ",
    /* HH */ "ヒ", /* JH */ "ジ", /* K */ "キ", /* L */ "リ", /* M */ "ミ", /* N */ "ニ",
    /* NG */ "ギ", /* P */ "ピ", /* R */ "リ", /* S */ "シ", /* SH */ "シ", /* T */ "テ",
    /* TH */ "シ", /* V */ "ビ", /* W */ "ウ", /* Y */ "", /* Z */ "ジ", /* ZH */ "ジ"};

// 母音の段（0=a 1=i 2=u 3=e 4=o）
int columnOf(uint8_t v, bool oSpelling) {
  switch (v) {
    case AA: return oSpelling ? 4 : 0;
    case AE: case AH: case ER: case AW: case AY: return 0;
    case EH: case EY: return 3;
    case IH: case IY: return 1;
    case UH: case UW: return 2;
    default: return 4;  // AO OW OY
  }
}

// 子音だけのとき（語末・子音の前）のカタカナ
const char* codaKana(uint8_t c) {
  switch (c) {
    case T: return "ト";
    case D: return "ド";
    case CH: return "チ";
    case JH: return "ジ";
    case SH: return "シュ";
    case ZH: return "ジュ";
    case TH: return "ス";
    case DH: return "ズ";
    case N: return "ン";
    case NG: return "ング";
    case L: return "ル";
    case R: return "ル";
    case W: return "ウ";
    case Y: return "イ";
    default: return kRows[c][2];  // ウ段（ク・ス・プ・ム・ブ…）
  }
}

bool isShortVowel(uint8_t v) {
  return v == AE || v == EH || v == IH || v == AH || v == AA || v == UH;
}

bool geminates(uint8_t c) {
  return c == P || c == T || c == K || c == CH || c == D || c == G || c == JH || c == SH;
}

const char* const kLetterNames[26] = {
    "エー", "ビー", "シー", "ディー", "イー", "エフ", "ジー", "エイチ", "アイ",
    "ジェー", "ケー", "エル", "エム", "エヌ", "オー", "ピー", "キュー", "アール",
    "エス", "ティー", "ユー", "ブイ", "ダブリュー", "エックス", "ワイ", "ゼット"};

bool hasVowelLetter(const std::string& s) {
  for (char c : s) {
    const char l = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (strchr("aeiouy", l)) return true;
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// EnDict
// ---------------------------------------------------------------------------

bool EnDict::open(const uint8_t* data, size_t size) {
  data_ = nullptr;
  if (data == nullptr || size < 32 || memcmp(data, "SCED", 4) != 0 || readU16(data + 4) != 1) {
    return false;
  }
  const uint32_t blockCount = readU32(data + 8);
  const uint32_t blockIndex = readU32(data + 12);
  const uint32_t keyIndex = readU32(data + 16);
  const uint32_t keyArea = readU32(data + 20);
  const uint32_t dataOffset = readU32(data + 24);
  ltsOffset_ = readU32(data + 28);
  if (dataOffset > size || ltsOffset_ > size) return false;
  blocks_.init(data, blockCount, blockIndex, keyIndex, keyArea, dataOffset);
  data_ = data;
  size_ = size;
  return true;
}

bool EnDict::lookup(const std::string& word, std::vector<uint8_t>& phones) {
  if (!isOpen() || word.empty() || word.size() > 255) return false;
  const uint8_t* key = reinterpret_cast<const uint8_t*>(word.data());
  const int bi = blocks_.findBlock(key, word.size());
  if (bi < 0) return false;
  size_t size = 0;
  const uint8_t* b = blocks_.block(static_cast<uint32_t>(bi), &size);
  if (b == nullptr) return false;
  uint8_t current[256];
  size_t currentLen = 0;
  size_t pos = 0;
  while (pos + 2 <= size) {
    const uint8_t prefix = b[pos];
    const uint8_t suffix = b[pos + 1];
    pos += 2;
    memcpy(current + prefix, b + pos, suffix);
    currentLen = prefix + suffix;
    pos += suffix;
    const uint8_t n = b[pos++];
    const int cmp = BlockStore::compareKeys(current, currentLen, key, word.size());
    if (cmp == 0) {
      phones.assign(b + pos, b + pos + n);
      return true;
    }
    if (cmp > 0) return false;
    pos += n;
  }
  return false;
}

bool EnDict::guess(const std::string& word, std::vector<uint8_t>& phones) const {
  if (!isOpen() || ltsOffset_ == 0) return false;
  const uint8_t* lts = data_ + ltsOffset_;
  const uint8_t depth = lts[0];
  const int8_t* offsets = reinterpret_cast<const int8_t*>(lts + 1);
  const uint8_t* classes = lts + 1 + depth + 4;
  const uint32_t classBytes = readU32(lts + 1 + depth);
  const uint8_t* tree = classes + classBytes;
  const uint16_t classCount = readU16(classes);

  auto letterIndex = [&](int i) -> uint8_t {
    if (i < 0 || i >= static_cast<int>(word.size())) return 0;  // '#'
    const char c = word[i];
    if (c >= 'a' && c <= 'z') return static_cast<uint8_t>(c - 'a' + 1);
    if (c == '\'') return 27;
    return 0;
  };

  for (int i = 0; i < static_cast<int>(word.size()); ++i) {
    const uint8_t* node = tree;
    for (uint8_t d = 0; d < depth; ++d) {
      const uint8_t value = letterIndex(i + offsets[d]);
      const uint8_t children = node[2];
      const uint8_t* child = nullptr;
      for (uint8_t k = 0; k < children; ++k) {
        const uint8_t* e = node + 3 + k * 4;
        if (e[0] == value) {
          child = tree + (e[1] | (e[2] << 8) | (e[3] << 16));
          break;
        }
      }
      if (child == nullptr) break;
      node = child;
    }
    const uint16_t cls = readU16(node);
    if (cls >= classCount) return false;
    // クラス表: [u16 数] 各クラス [u8 n][音素×n]（先頭から順に数える）
    const uint8_t* c = classes + 2;
    for (uint16_t k = 0; k < cls; ++k) c += 1 + c[0];
    phones.insert(phones.end(), c + 1, c + 1 + c[0]);
  }
  return true;
}

// ---------------------------------------------------------------------------
// EnReader
// ---------------------------------------------------------------------------

std::string EnReader::spell(const std::string& letters) {
  std::string out;
  for (char c : letters) {
    const char u = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    if (u >= 'A' && u <= 'Z') out += kLetterNames[u - 'A'];
  }
  return out;
}

std::string EnReader::phonesToKana(const std::vector<uint8_t>& codes, uint8_t* accent,
                                   bool oForAA) {
  std::vector<Phone> ph;
  for (uint8_t c : codes) ph.push_back(decodePhone(c));
  std::string out;
  int moraIndex = 0;      // 出力したモーラの数
  int stressedMora = -1;  // 第1強勢の母音のモーラ（0 始まり）
  bool lastLong = false;  // 直前のモーラが長音・二重母音で終わった
  const size_t n = ph.size();

  auto emit = [&](const std::string& kana, int moras) {
    out += kana;
    moraIndex += moras;
  };

  for (size_t i = 0; i < n; ++i) {
    const Phone& p = ph[i];
    uint8_t consonant = kNoConsonant;
    size_t vi = i;
    bool yoon = false;
    if (!p.vowel) {
      consonant = p.id;
      // 子音 ＋ Y ＋ 母音 は拗音（ピュー、キュー）
      if (i + 2 < n && !ph[i + 1].vowel && ph[i + 1].id == Y && ph[i + 2].vowel &&
          consonant != Y && consonant != W) {
        yoon = true;
        vi = i + 2;
      } else if (i + 1 < n && ph[i + 1].vowel) {
        vi = i + 1;
      } else {
        // 子音だけ（語末・子音の前）
        const bool afterVowel = i > 0 && ph[i - 1].vowel;
        const bool nextIsLabial = i + 1 < n && !ph[i + 1].vowel &&
                                  (ph[i + 1].id == B || ph[i + 1].id == P || ph[i + 1].id == M);
        if (consonant == R && afterVowel) {
          if (!lastLong) emit("ー", 1);  // car → カー
          lastLong = true;
          continue;
        }
        if (consonant == M && nextIsLabial) {
          emit("ン", 1);
          lastLong = false;
          continue;
        }
        if (afterVowel && geminates(consonant) && isShortVowel(ph[i - 1].id) &&
            ph[i - 1].stress > 0) {
          emit("ッ", 1);  // cat → キャット
        }
        // ts → ツ、dz → ズ
        if (consonant == T && i + 1 < n && !ph[i + 1].vowel && ph[i + 1].id == S &&
            (i + 2 >= n || !ph[i + 2].vowel)) {
          emit("ツ", 1);
          ++i;
          lastLong = false;
          continue;
        }
        if (consonant == D && i + 1 < n && !ph[i + 1].vowel && ph[i + 1].id == Z &&
            (i + 2 >= n || !ph[i + 2].vowel)) {
          emit("ズ", 1);
          ++i;
          lastLong = false;
          continue;
        }
        const char* coda = codaKana(consonant);
        std::vector<Mora> m;
        splitMoras(coda, m);
        emit(coda, static_cast<int>(m.size()));
        lastLong = false;
        continue;
      }
    }

    // 子音＋母音（または母音だけ）
    const Phone& v = ph[vi];
    const bool wordEnd = vi + 1 >= n;
    const bool nextIsVowel = !wordEnd && ph[vi + 1].vowel;
    if (v.stress == 1 && stressedMora < 0) stressedMora = moraIndex;
    std::string kana;
    const int col = columnOf(v.id, oForAA);
    if (yoon) {
      kana = kYoonBase[consonant];
      static const char* const kSmall[5] = {"ャ", "", "ュ", "ェ", "ョ"};
      if (col == 1) {
        kana = kRows[consonant][1];
      } else {
        kana += kSmall[col];
      }
    } else if ((consonant == K || consonant == G) && v.id == AE) {
      kana = consonant == K ? "キャ" : "ギャ";  // cat → キャット
    } else if (consonant == W && v.id == AA) {
      kana = "ウォ";  // watch → ウォッチ
    } else if (consonant == NG) {
      kana = std::string("ン") + kRows[NG][col];  // singer → シンガー
    } else {
      kana = kRows[consonant][col];
    }
    std::vector<Mora> m;
    splitMoras(kana, m);
    emit(kana, static_cast<int>(m.size()));
    lastLong = false;

    // 長音・二重母音
    switch (v.id) {
      case ER:
        emit("ー", 1);
        lastLong = true;
        break;
      case IY:
      case UW:
        if (v.stress > 0 || wordEnd) {
          emit("ー", 1);
          lastLong = true;
        }
        break;
      case OW:
        emit("ー", 1);
        lastLong = true;
        break;
      case AO:
        if (v.stress > 0) {
          emit("ー", 1);
          lastLong = true;
        }
        break;
      case AY:
      case OY:
        emit("イ", 1);
        lastLong = true;
        break;
      case AW:
        emit("ウ", 1);
        lastLong = true;
        break;
      case EY:
        emit(wordEnd || nextIsVowel ? "イ" : "ー", 1);  // day → デイ、game → ゲーム
        lastLong = true;
        break;
      default:
        break;
    }
    i = vi;
  }
  if (accent) *accent = stressedMora >= 0 ? static_cast<uint8_t>(stressedMora + 1) : 0;
  return out;
}

std::string EnReader::romajiToKana(const std::string& s) {
  // ヘボン式・訓令式のローマ字として全部読めたときだけカタカナにする
  static const struct { const char* roma; const char* kana; } kTable[] = {
      {"kya", "キャ"}, {"kyu", "キュ"}, {"kyo", "キョ"}, {"gya", "ギャ"}, {"gyu", "ギュ"},
      {"gyo", "ギョ"}, {"sha", "シャ"}, {"shu", "シュ"}, {"sho", "ショ"}, {"she", "シェ"},
      {"shi", "シ"}, {"cha", "チャ"}, {"chu", "チュ"}, {"cho", "チョ"}, {"che", "チェ"},
      {"chi", "チ"}, {"tsu", "ツ"}, {"nya", "ニャ"}, {"nyu", "ニュ"}, {"nyo", "ニョ"},
      {"hya", "ヒャ"}, {"hyu", "ヒュ"}, {"hyo", "ヒョ"}, {"bya", "ビャ"}, {"byu", "ビュ"},
      {"byo", "ビョ"}, {"pya", "ピャ"}, {"pyu", "ピュ"}, {"pyo", "ピョ"}, {"mya", "ミャ"},
      {"myu", "ミュ"}, {"myo", "ミョ"}, {"rya", "リャ"}, {"ryu", "リュ"}, {"ryo", "リョ"},
      {"sya", "シャ"}, {"syu", "シュ"}, {"syo", "ショ"}, {"tya", "チャ"}, {"tyu", "チュ"},
      {"tyo", "チョ"}, {"zya", "ジャ"}, {"zyu", "ジュ"}, {"zyo", "ジョ"},
      {"ja", "ジャ"}, {"ju", "ジュ"}, {"jo", "ジョ"}, {"je", "ジェ"}, {"ji", "ジ"},
      {"fa", "ファ"}, {"fi", "フィ"}, {"fu", "フ"}, {"fe", "フェ"}, {"fo", "フォ"},
      {"ka", "カ"}, {"ki", "キ"}, {"ku", "ク"}, {"ke", "ケ"}, {"ko", "コ"},
      {"ga", "ガ"}, {"gi", "ギ"}, {"gu", "グ"}, {"ge", "ゲ"}, {"go", "ゴ"},
      {"sa", "サ"}, {"si", "シ"}, {"su", "ス"}, {"se", "セ"}, {"so", "ソ"},
      {"za", "ザ"}, {"zi", "ジ"}, {"zu", "ズ"}, {"ze", "ゼ"}, {"zo", "ゾ"},
      {"ta", "タ"}, {"ti", "チ"}, {"tu", "ツ"}, {"te", "テ"}, {"to", "ト"},
      {"da", "ダ"}, {"di", "ディ"}, {"du", "ドゥ"}, {"de", "デ"}, {"do", "ド"},
      {"na", "ナ"}, {"ni", "ニ"}, {"nu", "ヌ"}, {"ne", "ネ"}, {"no", "ノ"},
      {"ha", "ハ"}, {"hi", "ヒ"}, {"hu", "フ"}, {"he", "ヘ"}, {"ho", "ホ"},
      {"ba", "バ"}, {"bi", "ビ"}, {"bu", "ブ"}, {"be", "ベ"}, {"bo", "ボ"},
      {"pa", "パ"}, {"pi", "ピ"}, {"pu", "プ"}, {"pe", "ペ"}, {"po", "ポ"},
      {"ma", "マ"}, {"mi", "ミ"}, {"mu", "ム"}, {"me", "メ"}, {"mo", "モ"},
      {"ya", "ヤ"}, {"yu", "ユ"}, {"yo", "ヨ"},
      {"ra", "ラ"}, {"ri", "リ"}, {"ru", "ル"}, {"re", "レ"}, {"ro", "ロ"},
      {"la", "ラ"}, {"li", "リ"}, {"lu", "ル"}, {"le", "レ"}, {"lo", "ロ"},
      {"wa", "ワ"}, {"wo", "ヲ"}, {"va", "ヴァ"}, {"vi", "ヴィ"}, {"vu", "ヴ"},
      {"a", "ア"}, {"i", "イ"}, {"u", "ウ"}, {"e", "エ"}, {"o", "オ"},
  };
  std::string out;
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    const char c = s[i];
    // 撥音: n の後が子音・語末（n' も）
    if (c == 'n' && (i + 1 >= n || s[i + 1] == '\'' ||
                     (!strchr("aiueoy", s[i + 1])))) {
      out += "ン";
      i += (i + 1 < n && s[i + 1] == '\'') ? 2 : 1;
      continue;
    }
    if (c == 'm' && i + 1 < n && strchr("bpm", s[i + 1])) {
      out += "ン";  // shimbun
      ++i;
      continue;
    }
    // 促音: 同じ子音の重なり（tch も）
    if (i + 1 < n && c == s[i + 1] && !strchr("aiueon", c)) {
      out += "ッ";
      ++i;
      continue;
    }
    if (c == 't' && i + 2 < n && s[i + 1] == 'c' && s[i + 2] == 'h') {
      out += "ッ";
      ++i;
      continue;
    }
    bool matched = false;
    for (const auto& e : kTable) {
      const size_t len = strlen(e.roma);
      if (s.compare(i, len, e.roma) == 0) {
        out += e.kana;
        i += len;
        matched = true;
        break;
      }
    }
    if (!matched) return std::string();
  }
  return out;
}

void EnReader::readPart(const std::string& part, std::vector<JaToken>& out) {
  if (part.empty()) return;
  JaToken tok;
  tok.latin = true;
  tok.pos = kPosNoun;
  for (char c : part) tok.surface += static_cast<uint32_t>(static_cast<unsigned char>(c));

  if (isdigit(static_cast<unsigned char>(part[0]))) {
    // 英字にくっついた数字: 1桁は英語（3D → スリー、P1S → ワン）、2桁以上は日本語（M400 → ヨンヒャク）
    if (part.size() == 1) {
      tok.pron = readNumberEn(part);
      tok.accent = 1;
    } else {
      const NumberReading r = readNumberJa(part);
      tok.pron = r.pron();
      tok.accent = r.accent;
    }
    out.push_back(tok);
    return;
  }

  std::string lower;
  bool allUpper = true;
  for (char c : part) {
    lower += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    allUpper &= isupper(static_cast<unsigned char>(c)) != 0;
  }

  // 大文字の略語（AMS, PLA, LED）・母音の無い語（mp, http）はアルファベット読み
  const bool acronym = (allUpper && part.size() >= 2 && part.size() <= 5) || !hasVowelLetter(part);
  // 綴りに o があって a が無ければ AA は「オ」（hot, top, doctor）
  const bool oForAA = lower.find('o') != std::string::npos && lower.find('a') == std::string::npos;
  std::vector<uint8_t> phones;
  if (!acronym && dict_ != nullptr && dict_->lookup(lower, phones)) {
    tok.pron = phonesToKana(phones, &tok.accent, oForAA);
  } else if (!acronym) {
    const std::string roma = romajiToKana(lower);
    if (!roma.empty()) {
      tok.pron = roma;
      tok.accent = 0;
    } else if (dict_ != nullptr && dict_->guess(lower, phones)) {
      tok.pron = phonesToKana(phones, &tok.accent, oForAA);
    }
  }
  if (tok.pron.empty()) {
    tok.pron = spell(part);
    // アルファベット読みは最後の文字の頭にアクセント（エーエムエ'ス）
    std::vector<Mora> all;
    splitMoras(tok.pron, all);
    std::vector<Mora> last;
    splitMoras(spell(part.substr(part.size() - 1)), last);
    tok.accent = static_cast<uint8_t>(all.size() - last.size() + 1);
  }
  out.push_back(tok);
}

void EnReader::read(const std::string& word, std::vector<JaToken>& out) {
  // 英字と数字の境目、キャメルケース（StackChan → Stack + Chan、HTMLParser → HTML + Parser）で分ける
  size_t start = 0;
  for (size_t i = 1; i <= word.size(); ++i) {
    bool cut = i == word.size();
    if (!cut) {
      const unsigned char a = static_cast<unsigned char>(word[i - 1]);
      const unsigned char b = static_cast<unsigned char>(word[i]);
      const bool aDigit = isdigit(a) != 0;
      const bool bDigit = isdigit(b) != 0;
      cut = aDigit != bDigit || (islower(a) && isupper(b)) ||
            (isupper(a) && isupper(b) && i + 1 < word.size() &&
             islower(static_cast<unsigned char>(word[i + 1])));
    }
    if (cut) {
      readPart(word.substr(start, i - start), out);
      start = i;
    }
  }
}

}  // namespace talk
