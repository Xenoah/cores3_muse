// PC で動く ReplySplitter のテスト。  python tools/reply_test/run.py
#include <cstdio>
#include <string>
#include <vector>

#include "ReplySplitter.h"

namespace {

struct Out {
  std::string text;
  Mood mood;
};

std::vector<Out> split(const std::vector<std::string>& fragments) {
  std::vector<Out> out;
  ReplySplitter splitter([&](const std::string& s, Mood m) { out.push_back({s, m}); });
  for (const std::string& fragment : fragments) splitter.feed(fragment.c_str());
  splitter.finish();
  return out;
}

int failures = 0;

void expect(const char* name, const std::vector<std::string>& fragments,
            const std::vector<Out>& expected) {
  const std::vector<Out> actual = split(fragments);
  bool ok = actual.size() == expected.size();
  for (size_t i = 0; ok && i < actual.size(); ++i) {
    ok = actual[i].text == expected[i].text && actual[i].mood == expected[i].mood;
  }
  if (ok) {
    printf("ok   %s\n", name);
    return;
  }
  ++failures;
  printf("FAIL %s\n", name);
  for (const Out& out : actual) printf("  got [%d] %s\n", static_cast<int>(out.mood), out.text.c_str());
}

// 1 バイトずつ届いても、まとめて届いても結果が変わらないこと
void expectChunkingIndependent(const char* name, const std::string& text) {
  std::vector<std::string> bytes;
  for (char c : text) bytes.push_back(std::string(1, c));
  const std::vector<Out> whole = split({text});
  const std::vector<Out> byByte = split(bytes);
  bool ok = whole.size() == byByte.size();
  for (size_t i = 0; ok && i < whole.size(); ++i) ok = whole[i].text == byByte[i].text;
  printf("%s %s\n", ok ? "ok  " : "FAIL", name);
  if (!ok) ++failures;
}

}  // namespace

int main() {
  expect("sentences", {"こんにちは。今日は", "いい天気だね！"},
         {{"こんにちは。", Mood::Neutral}, {"今日はいい天気だね！", Mood::Neutral}});
  expect("mood tag at start", {"[happy] やったね！"}, {{"やったね！", Mood::Happy}});
  expect("mood split over fragments", {"[ha", "ppy", "] や", "ったね。"},
         {{"やったね。", Mood::Happy}});
  expect("mood carries over", {"[sad] 残念。でも大丈夫。"},
         {{"残念。", Mood::Sad}, {"でも大丈夫。", Mood::Sad}});
  expect("mood changes", {"[happy] わあ。[doubt] 本当に？"},
         {{"わあ。", Mood::Happy}, {"本当に？", Mood::Doubt}});
  expect("mood only", {"[happy]"}, {});
  expect("unknown tag is kept", {"[メモ] 大事です。"}, {{"[メモ] 大事です。", Mood::Neutral}});
  expect("no terminator", {"おはよう"}, {{"おはよう", Mood::Neutral}});
  expect("decimal is not an end", {"円周率は3.14です。"}, {{"円周率は3.14です。", Mood::Neutral}});
  expect("version is not an end", {"Muse Spark 1.3 is new. OK."},
         {{"Muse Spark 1.3 is new.", Mood::Neutral}, {"OK.", Mood::Neutral}});
  expect("closing bracket stays", {"「すごい！」と言った。"},
         {{"「すごい！」", Mood::Neutral}, {"と言った。", Mood::Neutral}});
  expect("repeated marks", {"えっ！？ほんと？"},
         {{"えっ！？", Mood::Neutral}, {"ほんと？", Mood::Neutral}});
  expect("markdown removed", {"**大事**なのは `これ` です。"},
         {{"大事なのは これ です。", Mood::Neutral}});
  expect("emoji removed", {"うれしい😀！"}, {{"うれしい！", Mood::Neutral}});
  expect("url removed", {"詳しくは https://example.com/a?b=1 を見てね。"},
         {{"詳しくは  を見てね。", Mood::Neutral}});
  expect("newlines split", {"一行目\n二行目"},
         {{"一行目", Mood::Neutral}, {"二行目", Mood::Neutral}});
  expect("empty", {""}, {});

  // 長い文は読点で分かれ、字幕の 255 バイトに収まる
  std::string longText;
  for (int i = 0; i < 30; ++i) longText += "とても長い文章です、";
  const std::vector<Out> pieces = split({longText});
  bool sized = pieces.size() > 1;
  std::string joined;
  for (const Out& piece : pieces) {
    sized = sized && piece.text.size() <= 250;
    joined += piece.text;
  }
  sized = sized && joined == longText.substr(0, longText.size());
  printf("%s long sentence is split at commas (%zu pieces)\n", sized ? "ok  " : "FAIL", pieces.size());
  if (!sized) ++failures;

  // 読点も句点もない長い文でも、UTF-8 の途中で切らない
  std::string noBreak;
  for (int i = 0; i < 120; ++i) noBreak += "あ";
  const std::vector<Out> hard = split({noBreak});
  bool hardOk = !hard.empty();
  for (const Out& piece : hard) hardOk = hardOk && piece.text.size() % 3 == 0 && piece.text.size() <= 250;
  printf("%s long text without commas keeps UTF-8 intact (%zu pieces)\n", hardOk ? "ok  " : "FAIL", hard.size());
  if (!hardOk) ++failures;

  expectChunkingIndependent("byte-by-byte: japanese",
                            "[happy] こんにちは！今日は「いい天気」だね。3.14 は円周率です。");
  expectChunkingIndependent("byte-by-byte: english", "[sad] Oh no. That is v1.2 of it! Really?");

  printf(failures == 0 ? "all passed\n" : "%d failed\n", failures);
  return failures == 0 ? 0 : 1;
}
