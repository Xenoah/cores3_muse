#include "JaReader.h"

#include <algorithm>
#include <climits>

namespace talk {

namespace {

// MeCab（unk.def）の未知語コスト
constexpr int32_t kUnknownKatakanaCost = 8360;
constexpr int32_t kUnknownHiraganaCost = 12341;
constexpr int32_t kUnknownKanjiCost = 10654;
constexpr int32_t kUnknownOtherCost = 12000;

enum class CharClass { Hiragana, Katakana, Kanji, Other };

CharClass classOf(uint32_t c) {
  if (isHiragana(c)) return CharClass::Hiragana;
  if (isKatakana(c)) return CharClass::Katakana;
  if (isKanji(c)) return CharClass::Kanji;
  return CharClass::Other;
}

std::string kataOf(const std::u32string& s) {
  std::string out;
  for (uint32_t c : s) appendUtf8(out, hiraToKata(c));
  return out;
}

}  // namespace

void JaReader::addUnknown(const std::u32string& text, int start, int end,
                          int32_t cost, std::vector<Node>& nodes) {
  Node n;
  n.start = start;
  n.end = end;
  n.entry.type = dict_.unknownType();
  n.entry.accent = 0;
  n.entry.cost = static_cast<int16_t>(cost);
  const std::u32string s = text.substr(start, end - start);
  // かなはそのまま読む。読めない漢字などは無音にする
  bool kana = true;
  for (uint32_t c : s) kana &= isHiragana(c) || isKatakana(c);
  if (kana) n.entry.pron = kataOf(s);
  n.best = INT32_MAX;
  n.prev = -1;
  nodes.push_back(n);
}

void JaReader::tokenize(const std::u32string& text, Context context,
                        std::vector<JaToken>& out) {
  const int n = static_cast<int>(text.size());
  if (n == 0) return;

  // 数や英単語のあとは、文頭をその語の右文脈にする（助数詞・助詞の読みを選ばせる）
  uint8_t bosRight = dict_.bosRightCluster();
  if (context == Context::Noun) {
    bosRight = dict_.type(dict_.unknownType()).rightCluster;
  } else if (context == Context::Number) {
    static int numberCluster = -1;
    if (numberCluster < 0) {
      std::string key;
      std::vector<JaEntry> found;
      const uint32_t one = 0x4E00;  // 一
      if (dict_.encodeChar(one, key)) dict_.lookup(key, &one, 1, found);
      numberCluster = 0;
      for (const JaEntry& e : found) {
        if (dict_.type(e.type).group == kGroupNumber) {
          numberCluster = dict_.type(e.type).rightCluster;
          break;
        }
      }
    }
    if (numberCluster > 0) bosRight = static_cast<uint8_t>(numberCluster);
  }

  std::vector<Node> nodes;
  std::vector<std::vector<int>> endsAt(n + 1);
  std::vector<JaEntry> found;

  for (int i = 0; i < n; ++i) {
    if (i > 0 && endsAt[i].empty()) continue;  // ここで終わる語が無い（到達できない）
    const size_t firstNew = nodes.size();

    // 辞書の前方一致
    std::string key;
    bool matchedOne = false;
    const int maxLen = std::min<int>(dict_.maxChars(), n - i);
    for (int len = 1; len <= maxLen; ++len) {
      if (!dict_.encodeChar(text[i + len - 1], key)) break;
      found.clear();
      const bool longer = dict_.lookup(key, reinterpret_cast<const uint32_t*>(text.data()) + i,
                                       len, found);
      for (const JaEntry& e : found) {
        Node node;
        node.start = i;
        node.end = i + len;
        node.entry = e;
        node.best = INT32_MAX;
        node.prev = -1;
        nodes.push_back(node);
        if (len == 1) matchedOne = true;
      }
      if (!longer) break;
    }
    const bool matchedAny = nodes.size() > firstNew;

    // 未知語（MeCab の unk.def と同じ扱い）
    const CharClass cls = classOf(text[i]);
    int run = i;
    while (run < n && classOf(text[run]) == cls) ++run;
    if (cls == CharClass::Katakana) {
      // カタカナは続く限り1語にまとめる（常に候補にする）
      addUnknown(text, i, run, kUnknownKatakanaCost, nodes);
      if (!matchedOne && run - i > 1) addUnknown(text, i, i + 1, kUnknownKatakanaCost, nodes);
    } else if (!matchedAny) {
      if (cls == CharClass::Hiragana) {
        addUnknown(text, i, i + 1, kUnknownHiraganaCost, nodes);
        if (run - i >= 2) addUnknown(text, i, i + 2, kUnknownHiraganaCost, nodes);
      } else if (cls == CharClass::Kanji) {
        addUnknown(text, i, i + 1, kUnknownKanjiCost, nodes);
        if (run - i >= 2) addUnknown(text, i, i + 2, kUnknownKanjiCost, nodes);
      } else {
        addUnknown(text, i, i + 1, kUnknownOtherCost, nodes);
      }
    }

    // 新しい語のコストを決める（ここで終わる語から最良のものをつなぐ）
    for (size_t k = firstNew; k < nodes.size(); ++k) {
      Node& node = nodes[k];
      const JaType& t = dict_.type(node.entry.type);
      int32_t best = INT32_MAX;
      int prev = -1;
      if (i == 0) {
        best = dict_.connCost(bosRight, t.leftCluster);
      } else {
        for (int p : endsAt[i]) {
          const Node& pn = nodes[p];
          const int32_t c =
              pn.best + dict_.connCost(dict_.type(pn.entry.type).rightCluster, t.leftCluster);
          if (c < best) {
            best = c;
            prev = p;
          }
        }
      }
      node.best = best + node.entry.cost;
      node.prev = prev;
      endsAt[node.end].push_back(static_cast<int>(k));
    }
  }

  // 文末につなぐ
  int32_t best = INT32_MAX;
  int last = -1;
  for (int p : endsAt[n]) {
    const Node& pn = nodes[p];
    const int32_t c = pn.best + dict_.connCost(dict_.type(pn.entry.type).rightCluster,
                                               dict_.eosLeftCluster());
    if (c < best) {
      best = c;
      last = p;
    }
  }
  std::vector<int> path;
  for (int k = last; k >= 0; k = nodes[k].prev) path.push_back(k);

  for (auto it = path.rbegin(); it != path.rend(); ++it) {
    const Node& node = nodes[*it];
    const JaType& t = dict_.type(node.entry.type);
    JaToken tok;
    tok.surface = text.substr(node.start, node.end - node.start);
    tok.pron = node.entry.pron;
    tok.accent = node.entry.accent;
    tok.pos = t.pos;
    tok.group = t.group;
    tok.renyou = t.renyou;
    tok.rule = t.rule;
    // 「しょ」「ましょ」＋助動詞「う」→ 長音（しましょう → シマショー）
    if (tok.pos == kPosAuxiliary && tok.surface == U"う" && !out.empty()) {
      std::vector<Mora> prevMoras;
      splitMoras(out.back().pron, prevMoras);
      if (!prevMoras.empty() && moraVowel(prevMoras.back().kana) == 'o') tok.pron = "ー";
    }
    out.push_back(tok);
  }
}

bool JaReader::chains(const JaToken& prev, const JaToken& cur) const {
  if (prev.latin && cur.latin) return false;  // 英文は1語ずつ句にする
  // Open JTalk njd_set_accent_phrase の規則（番号はその Rule 番号）
  bool flag = false;                                                           // 01
  if (prev.pos == kPosNoun && cur.pos == kPosNoun) flag = true;                // 02
  if (prev.pos == kPosAdjective && cur.pos == kPosNoun) flag = false;          // 03
  if (prev.group == kGroupAdjectivalNoun && cur.pos == kPosNoun) flag = false; // 04
  if (prev.pos == kPosVerb && (cur.pos == kPosAdjective || cur.pos == kPosNoun)) flag = false;  // 05
  auto independent = [](uint8_t pos) {
    return pos == kPosAdverb || pos == kPosConjunction || pos == kPosAdnominal;
  };
  if (independent(cur.pos) || independent(prev.pos)) flag = false;             // 06
  if ((cur.pos == kPosNoun && cur.group == kGroupAdverbial) ||
      (prev.pos == kPosNoun && prev.group == kGroupAdverbial)) {
    flag = false;                                                              // 07
  }
  auto ancillary = [](uint8_t pos) { return pos == kPosParticle || pos == kPosAuxiliary; };
  if (ancillary(cur.pos)) flag = true;                                         // 08
  if (ancillary(prev.pos) && !ancillary(cur.pos)) flag = false;                // 09
  if (prev.group == kGroupSuffix && cur.pos == kPosNoun) flag = false;         // 10
  if (cur.pos == kPosAdjective && cur.group == kGroupDependent) {              // 11
    if (((prev.pos == kPosVerb || prev.pos == kPosAdjective) && prev.renyou) ||
        (prev.pos == kPosParticle && prev.group == kGroupConjunctive &&
         (prev.surface == U"て" || prev.surface == U"で"))) {
      flag = true;
    }
  }
  if (cur.pos == kPosVerb && cur.group == kGroupDependent) {                   // 12
    if ((prev.pos == kPosVerb && prev.renyou) ||
        (prev.pos == kPosNoun && prev.group == kGroupSahen)) {
      flag = true;
    }
  }
  if (prev.pos == kPosNoun &&
      (cur.pos == kPosVerb || cur.pos == kPosAdjective || cur.group == kGroupAdjectivalNoun)) {
    flag = false;                                                              // 13
  }
  if (cur.pos == kPosSymbol || prev.pos == kPosSymbol) flag = false;           // 14
  if (prev.pos == kPosPrefix) flag = true;                                     // 15（接頭詞は次の語につく）
  if (cur.group == kGroupSuffix) flag = true;                                  // 18
  return flag;
}

void JaReader::applyRule(const JaToken& prev, const JaToken& cur, int moraSize,
                         int* accent) const {
  uint8_t code = cur.fixedRule;
  int8_t add = 0;
  if (cur.rule != 0xFFFF) dict_.rule(cur.rule, prev.pos, &code, &add);
  const int acc = *accent;
  switch (code) {
    case kRuleF2: if (acc == 0) *accent = moraSize + add; break;
    case kRuleF3: if (acc != 0) *accent = moraSize + add; break;
    case kRuleF4: *accent = moraSize + add; break;
    case kRuleF5: *accent = 0; break;
    case kRuleC1: *accent = moraSize + cur.accent; break;
    case kRuleC2: *accent = moraSize + 1; break;
    case kRuleC3: *accent = moraSize; break;
    case kRuleC4: *accent = 0; break;
    case kRuleP1: *accent = cur.accent == 0 ? 0 : moraSize + cur.accent; break;
    case kRuleP2: *accent = cur.accent == 0 ? moraSize + 1 : moraSize + cur.accent; break;
    case kRuleP6: *accent = 0; break;
    case kRuleP14: if (cur.accent != 0) *accent = moraSize + cur.accent; break;
    default: break;  // *, F1, C5: そのまま
  }
}

void JaReader::buildPhrases(const std::vector<JaToken>& tokens, Utterance& utterance) {
  const JaToken* prev = nullptr;
  AccentPhrase* phrase = nullptr;
  for (const JaToken& tok : tokens) {
    std::vector<Mora> moras;
    splitMoras(tok.pron, moras);
    if (moras.empty()) continue;  // 読めない語（未知の漢字など）は飛ばす
    if (phrase == nullptr || prev == nullptr || !chains(*prev, tok)) {
      utterance.emplace_back();
      phrase = &utterance.back();
      phrase->accent = tok.accent;
    } else {
      int accent = phrase->accent;
      applyRule(*prev, tok, static_cast<int>(phrase->moras.size()), &accent);
      phrase->accent = accent;
    }
    phrase->moras.insert(phrase->moras.end(), moras.begin(), moras.end());
    if (phrase->accent < 0 || phrase->accent > static_cast<int>(phrase->moras.size())) {
      phrase->accent = 0;
    }
    prev = &tok;
  }
}

void markDevoicing(Utterance& utterance) {
  // 句をまたいでモーラを並べて判定する
  struct Ref {
    AccentPhrase* phrase;
    size_t index;
  };
  std::vector<Ref> refs;
  for (AccentPhrase& ph : utterance) {
    for (size_t i = 0; i < ph.moras.size(); ++i) refs.push_back({&ph, i});
  }
  for (size_t k = 0; k < refs.size(); ++k) {
    Mora& m = refs[k].phrase->moras[refs[k].index];
    if (m.devoiced) continue;
    const char v = moraVowel(m.kana);
    if ((v != 'i' && v != 'u') || !moraVoicelessConsonant(m.kana)) continue;
    // アクセント核のモーラは無声化しない
    if (refs[k].phrase->accent == static_cast<int>(refs[k].index) + 1) continue;
    if (k > 0 && refs[k - 1].phrase->moras[refs[k - 1].index].devoiced) continue;
    if (k + 1 < refs.size()) {
      // 次のモーラも無声子音で始まり、句の間に休みが無い
      const bool pauseBetween =
          refs[k + 1].phrase != refs[k].phrase && refs[k].phrase->pauseAfterMs > 0;
      const Mora& next = refs[k + 1].phrase->moras[refs[k + 1].index];
      if (!pauseBetween && moraVoicelessConsonant(next.kana)) m.devoiced = true;
    }
  }
}

}  // namespace talk
