#include "ReplySplitter.h"

#include <cstring>

namespace {

constexpr size_t kSoftLimit = 150;  // これより長い文は読点で分ける（字幕にも収める）
constexpr size_t kHardLimit = 240;  // 読点が無くても分ける

size_t utf8Length(uint8_t lead) {
  if (lead >= 0xF0) return 4;
  if (lead >= 0xE0) return 3;
  if (lead >= 0xC0) return 2;
  return 1;
}

// 文の終わりになる全角の記号
bool isFullWidthEnd(const char* s, size_t n) {
  return n == 3 && (!memcmp(s, "。", 3) || !memcmp(s, "！", 3) || !memcmp(s, "？", 3));
}

// 文の終わりの直後に続けて文に含める閉じ括弧など
bool isCloser(const char* s, size_t n) {
  if (n == 1) return *s == ')' || *s == '"' || *s == '\'';
  return n == 3 && (!memcmp(s, "」", 3) || !memcmp(s, "』", 3) || !memcmp(s, "）", 3) ||
                    !memcmp(s, "”", 3));
}

bool isAsciiSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// 文の切れ目の次の位置を返す。text の終端まで見ても確定できなければ npos。
// final なら終端も文の切れ目として扱う。
size_t sentenceEnd(const std::string& text, bool final) {
  size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    const size_t n = utf8Length(static_cast<uint8_t>(c));
    bool end = false;
    // URL の中の「?」や「.」は文の終わりではない
    if (text.compare(i, 7, "http://") == 0 || text.compare(i, 8, "https://") == 0) {
      while (i < text.size() && static_cast<uint8_t>(text[i]) < 0x80 && !isAsciiSpace(text[i])) ++i;
      if (i >= text.size() && !final) return std::string::npos;
      continue;
    }
    if (c == '\n') {
      return i + 1;
    } else if (c == '!' || c == '?') {
      end = true;
    } else if (c == '.') {
      // 3.14 や v1.2 の「.」は文の終わりではない
      if (i + 1 >= text.size()) {
        if (!final) return std::string::npos;
        end = true;
      } else {
        end = isAsciiSpace(text[i + 1]);
      }
    } else if (n == 3 && i + 3 <= text.size()) {
      end = isFullWidthEnd(text.data() + i, 3);
    }
    if (!end) {
      i += n;
      continue;
    }
    // 「！？」や「。」」のような続きも同じ文に含める
    size_t j = i + n;
    while (j < text.size()) {
      const size_t m = utf8Length(static_cast<uint8_t>(text[j]));
      if (j + m > text.size()) return final ? text.size() : std::string::npos;
      const bool more = text[j] == '!' || text[j] == '?' || text[j] == '.' ||
                        isFullWidthEnd(text.data() + j, m) || isCloser(text.data() + j, m);
      if (!more) return j;
      j += m;
    }
    // ここで文字列が切れている。次の断片が閉じ括弧かもしれないので待つ
    return final ? j : std::string::npos;
  }
  return std::string::npos;
}

// 長い文の分け目を探す
size_t softBreak(const std::string& text) {
  size_t best = 0;
  size_t i = 0;
  while (i < text.size() && i < kHardLimit) {
    const size_t n = utf8Length(static_cast<uint8_t>(text[i]));
    if (i + n > text.size()) break;
    const bool comma = (n == 3 && !memcmp(text.data() + i, "、", 3)) || text[i] == ',' ||
                       text[i] == ' ';
    if (comma && i + n >= kSoftLimit / 2) {
      best = i + n;
      if (i + n >= kSoftLimit) return best;
    }
    if (i + n <= kHardLimit && best == 0 && i + n >= kHardLimit - 3) best = i + n;
    i += n;
  }
  return best;
}

bool parseMood(const std::string& name, Mood& mood) {
  if (name == "happy") mood = Mood::Happy;
  else if (name == "sad") mood = Mood::Sad;
  else if (name == "angry") mood = Mood::Angry;
  else if (name == "doubt") mood = Mood::Doubt;
  else if (name == "sleepy") mood = Mood::Sleepy;
  else if (name == "neutral") mood = Mood::Neutral;
  else return false;
  return true;
}

}  // namespace

std::string cleanForSpeech(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    const uint8_t c = static_cast<uint8_t>(text[i]);
    const size_t n = utf8Length(c);
    // URL は読み上げても意味がないので落とす（ASCII が続く間）
    if (text.compare(i, 7, "http://") == 0 || text.compare(i, 8, "https://") == 0) {
      while (i < text.size() && static_cast<uint8_t>(text[i]) < 0x80 && !isAsciiSpace(text[i])) ++i;
      continue;
    }
    if (n == 4) {  // 絵文字
      i += 4;
      continue;
    }
    if (n == 3 && i + 3 <= text.size() &&
        (!memcmp(text.data() + i, "\xEF\xB8\x8F", 3) ||   // 異体字セレクタ
         !memcmp(text.data() + i, "\xE2\x80\x8D", 3))) {  // ゼロ幅接合子
      i += 3;
      continue;
    }
    if (c == '*' || c == '#' || c == '`' || c == '~' || c == '|' || c == '_') {  // Markdown
      ++i;
      continue;
    }
    if (c == '\n' || c == '\r' || c == '\t') {
      out += ' ';
      ++i;
      continue;
    }
    out.append(text, i, n);
    i += n;
  }
  const size_t first = out.find_first_not_of(' ');
  if (first == std::string::npos) return "";
  const size_t last = out.find_last_not_of(' ');
  return out.substr(first, last - first + 1);
}

void ReplySplitter::emitSentence(std::string sentence) {
  sentence = cleanForSpeech(sentence);
  // 文頭の気持ちのタグ。次の文にも引き継ぐ
  while (sentence.size() >= 2 && sentence[0] == '[') {
    const size_t close = sentence.find(']');
    if (close == std::string::npos) break;
    Mood mood;
    if (!parseMood(sentence.substr(1, close - 1), mood)) break;
    mood_ = mood;
    sentence = cleanForSpeech(sentence.substr(close + 1));
  }
  if (sentence.empty()) return;
  emit_(sentence, mood_);
}

void ReplySplitter::drain(bool final) {
  for (;;) {
    const size_t end = sentenceEnd(pending_, final);
    if (end != std::string::npos) {
      emitSentence(pending_.substr(0, end));
      pending_.erase(0, end);
      continue;
    }
    if (pending_.size() >= kSoftLimit) {
      const size_t cut = softBreak(pending_);
      if (cut > 0) {
        emitSentence(pending_.substr(0, cut));
        pending_.erase(0, cut);
        continue;
      }
    }
    break;
  }
}

void ReplySplitter::feed(const char* fragment) {
  pending_ += fragment;
  drain(false);
}

void ReplySplitter::finish() {
  drain(true);
  if (!pending_.empty()) emitSentence(pending_);
  pending_.clear();
}
