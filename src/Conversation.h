#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <deque>

#include "CameraView.h"
#include "MuseClient.h"
#include "ReplySplitter.h"
#include "Settings.h"

// 会話の処理が進むたびにメインループへ届く知らせ
struct ConversationEvent {
  enum class Type : uint8_t {
    ApiChecked,  // 接続確認の結果（result）
    Transcript,  // 聞き取った言葉（text）
    NoSpeech,    // 文字起こしの結果が空だった
    Sentence,    // 読み上げる 1 文（text, mood）
    Done,        // 返事が終わった
    Failed,      // 失敗した（result）
  };
  Type type = Type::Done;
  uint32_t generation = 0;
  String text;
  Mood mood = Mood::Neutral;
  MuseResult result;
};

// Muse とのやりとりを専用タスクで行う。TLS の通信中も画面と声を止めないため。
//
//   音声 → POST /asr/transcribe → 文字 → POST /chat/completions → 文ごとに読み上げ
class Conversation {
 public:
  void begin(MuseClient& muse, const Settings& settings);

  void checkApi();
  void submitAudio(const int16_t* pcm, size_t samples, uint32_t sampleRate);
  // 文を Muse に尋ねる。photo を付けると写真も見せる
  void submitText(const String& text, PhotoPtr photo = nullptr);

  // 進行中の処理と届いていない知らせを捨てる
  void cancel();
  // 覚えている会話を忘れる
  void forget();

  // 届いた知らせを 1 つ取り出す（無ければ nullptr）。受け取った側で delete する
  ConversationEvent* nextEvent();

 private:
  enum class JobType : uint8_t { None, Check, Audio, Text };
  struct Job {
    JobType type = JobType::None;
    uint32_t generation = 0;
    const int16_t* pcm = nullptr;
    size_t samples = 0;
    uint32_t sampleRate = 0;
    String text;
    PhotoPtr photo;
  };
  struct Turn {
    String user;
    String assistant;
  };

  static void taskEntry(void* self);
  void run();
  void submit(const Job& job);
  void process(const Job& job);
  void ask(const Job& job);
  String buildRequest(const Job& job) const;
  void post(ConversationEvent::Type type, uint32_t generation, const String& text = String(),
            Mood mood = Mood::Neutral, const MuseResult* result = nullptr);

  MuseClient* muse_ = nullptr;
  const Settings* settings_ = nullptr;
  TaskHandle_t task_ = nullptr;
  QueueHandle_t events_ = nullptr;
  SemaphoreHandle_t lock_ = nullptr;
  Job pending_;
  volatile uint32_t generation_ = 0;
  std::deque<Turn> history_;  // 作業タスクだけが触る
  volatile bool forgetRequested_ = false;
};
