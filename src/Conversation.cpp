#include "Conversation.h"

#include <ArduinoJson.h>
#include <time.h>

namespace {

constexpr size_t kMaxTurns = 6;            // 覚えておく往復の数
constexpr int kMaxCompletionTokens = 800;  // 考える分も含む上限（会話が切れない程度に）
constexpr uint32_t kTaskStackBytes = 16384;  // TLS のハンドシェイクは深いスタックを使う
constexpr TickType_t kPostWait = pdMS_TO_TICKS(200);

constexpr char kSystemPrompt[] =
    "あなたは机の上にいる小さなロボット「スタックチャン」です。"
    "マイクで聞いた言葉に、スピーカーから声で答えます。\n"
    "・親しみやすい話し言葉で話します。相手が日本語以外で話したら、その言語で答えます。\n"
    "・ふつうは1〜3文で短く答えます。詳しく頼まれたときだけ長く話します。\n"
    "・声で読み上げるので、Markdown、箇条書き、記号、絵文字、URLは使いません。\n"
    "・最初に今の気持ちを [happy] [sad] [angry] [doubt] [sleepy] [neutral] のどれか1つで書き、"
    "続けて話します。\n"
    "・聞き取りの誤りがありそうなときは文脈から考え、どうしても分からなければ聞き返します。\n";

String currentTimeText() {
  const time_t now = time(nullptr);
  struct tm local;
  localtime_r(&now, &local);
  char text[48];
  strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
  return text;
}

}  // namespace

void Conversation::begin(MuseClient& muse, const Settings& settings) {
  muse_ = &muse;
  settings_ = &settings;
  events_ = xQueueCreate(16, sizeof(ConversationEvent*));
  lock_ = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(taskEntry, "conversation", kTaskStackBytes, this, 1, &task_, PRO_CPU_NUM);
}

void Conversation::taskEntry(void* self) {
  static_cast<Conversation*>(self)->run();
}

void Conversation::submit(const Job& job) {
  xSemaphoreTake(lock_, portMAX_DELAY);
  pending_ = job;
  pending_.generation = ++generation_;
  xSemaphoreGive(lock_);
  xTaskNotifyGive(task_);
}

void Conversation::checkApi() {
  Job job;
  job.type = JobType::Check;
  submit(job);
}

void Conversation::submitAudio(const int16_t* pcm, size_t samples, uint32_t sampleRate) {
  Job job;
  job.type = JobType::Audio;
  job.pcm = pcm;
  job.samples = samples;
  job.sampleRate = sampleRate;
  submit(job);
}

void Conversation::submitText(const String& text) {
  Job job;
  job.type = JobType::Text;
  job.text = text;
  submit(job);
}

void Conversation::cancel() {
  xSemaphoreTake(lock_, portMAX_DELAY);
  ++generation_;
  pending_.type = JobType::None;
  xSemaphoreGive(lock_);
  muse_->cancel();
  // 古い知らせはここで捨てる。処理中のタスクが後から送るものは generation で見分ける
  ConversationEvent* event;
  while (xQueueReceive(events_, &event, 0) == pdTRUE) delete event;
}

void Conversation::forget() {
  forgetRequested_ = true;
}

ConversationEvent* Conversation::nextEvent() {
  ConversationEvent* event;
  while (xQueueReceive(events_, &event, 0) == pdTRUE) {
    if (event->generation == generation_) return event;
    delete event;
  }
  return nullptr;
}

void Conversation::post(ConversationEvent::Type type, uint32_t generation, const String& text,
                        Mood mood, const MuseResult* result) {
  auto* event = new ConversationEvent;
  event->type = type;
  event->generation = generation;
  event->text = text;
  event->mood = mood;
  if (result != nullptr) event->result = *result;
  // キューがいっぱいでも、取り消されたらすぐ諦める
  while (xQueueSend(events_, &event, kPostWait) != pdTRUE) {
    if (generation != generation_) {
      delete event;
      return;
    }
  }
}

void Conversation::run() {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    for (;;) {
      xSemaphoreTake(lock_, portMAX_DELAY);
      const Job job = pending_;
      pending_.type = JobType::None;
      muse_->clearCancel();
      xSemaphoreGive(lock_);
      if (job.type == JobType::None) break;
      process(job);
    }
  }
}

void Conversation::process(const Job& job) {
  if (forgetRequested_) {
    forgetRequested_ = false;
    history_.clear();
  }
  muse_->setApiKey(settings_->apiKey);

  if (job.type == JobType::Check) {
    int models = 0;
    const MuseResult result = muse_->listModels(&models);
    Serial.printf("[muse] check: status=%d ok=%d code=%s message=%s models=%d heap=%u\n",
                  result.status, result.ok, result.code.c_str(), result.message.c_str(), models,
                  static_cast<unsigned>(ESP.getFreeHeap()));
    post(ConversationEvent::Type::ApiChecked, job.generation, String(), Mood::Neutral, &result);
    return;
  }

  if (job.type == JobType::Text) {
    ask(job, job.text);
    return;
  }

  String transcript;
  const uint32_t started = millis();
  const MuseResult result = muse_->transcribe(job.pcm, job.samples, job.sampleRate, transcript);
  Serial.printf("[muse] transcribe: %.1fs audio, %ums, status=%d ok=%d code=%s\n",
                static_cast<float>(job.samples) / job.sampleRate,
                static_cast<unsigned>(millis() - started), result.status, result.ok,
                result.code.c_str());
  if (result.cancelled) return;
  if (!result.ok) {
    Serial.printf("[muse] transcribe failed: %s\n", result.message.c_str());
    post(ConversationEvent::Type::Failed, job.generation, String(), Mood::Neutral, &result);
    return;
  }
  if (transcript.isEmpty()) {
    post(ConversationEvent::Type::NoSpeech, job.generation);
    return;
  }
  post(ConversationEvent::Type::Transcript, job.generation, transcript);
  ask(job, transcript);
}

String Conversation::buildRequest(const String& userText) const {
  JsonDocument doc;
  doc["model"] = settings_->model;
  doc["reasoning_effort"] = settings_->reasoning;
  doc["max_completion_tokens"] = kMaxCompletionTokens;
  doc["stream"] = true;
  JsonArray messages = doc["messages"].to<JsonArray>();
  JsonObject system = messages.add<JsonObject>();
  system["role"] = "system";
  system["content"] = String(kSystemPrompt) + "・今の日時は " + currentTimeText() + " です。\n";
  for (const Turn& turn : history_) {
    JsonObject user = messages.add<JsonObject>();
    user["role"] = "user";
    user["content"] = turn.user;
    JsonObject assistant = messages.add<JsonObject>();
    assistant["role"] = "assistant";
    assistant["content"] = turn.assistant;
  }
  JsonObject user = messages.add<JsonObject>();
  user["role"] = "user";
  user["content"] = userText;
  String body;
  serializeJson(doc, body);
  return body;
}

void Conversation::ask(const Job& job, const String& userText) {
  const uint32_t started = millis();
  uint32_t firstTextAt = 0;
  String spoken;
  ReplySplitter splitter([&](const std::string& sentence, Mood mood) {
    spoken += sentence.c_str();
    post(ConversationEvent::Type::Sentence, job.generation, sentence.c_str(), mood);
  });
  const MuseResult result = muse_->chat(buildRequest(userText), [&](const char* delta) {
    if (firstTextAt == 0) firstTextAt = millis();
    splitter.feed(delta);
  });
  splitter.finish();
  Serial.printf("[muse] chat: first text %ums, total %ums, status=%d ok=%d code=%s heap=%u\n",
                static_cast<unsigned>(firstTextAt ? firstTextAt - started : 0),
                static_cast<unsigned>(millis() - started), result.status, result.ok,
                result.code.c_str(), static_cast<unsigned>(ESP.getFreeHeap()));
  if (result.cancelled) return;
  if (!result.ok) {
    Serial.printf("[muse] chat failed: %s\n", result.message.c_str());
    post(ConversationEvent::Type::Failed, job.generation, String(), Mood::Neutral, &result);
    return;
  }
  if (!spoken.isEmpty()) {
    history_.push_back({userText, spoken});
    while (history_.size() > kMaxTurns) history_.pop_front();
  }
  post(ConversationEvent::Type::Done, job.generation);
}
