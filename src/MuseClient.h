#pragma once

#include <Arduino.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include <functional>

// API 呼び出しの結果
struct MuseResult {
  bool ok = false;
  bool cancelled = false;
  int status = 0;   // HTTP ステータス（0 = サーバーまで届かなかった）
  String code;      // API のエラーコード（例: invalid_api_key）
  String message;   // エラーの詳細（シリアルログ用）
};

// Meta Model API（https://api.meta.ai/v1）のクライアント。ブロッキングなので専用タスクから呼ぶ。
//   GET  /models                一覧（接続確認に使う）
//   POST /asr/transcribe        音声 → 文字（muse-voice-transcribe-1.0）
//   POST /chat/completions      会話（SSE ストリーミング、画像は data URL）
class MuseClient {
 public:
  static constexpr const char* kDefaultBaseUrl = "https://api.meta.ai/v1";

  void setApiKey(const String& key) { apiKey_ = key; }

  // テスト用に接続先を差し替える（再起動で既定に戻る）。
  // 既定以外の接続先へ本物のキーは送らない。空文字列で既定に戻す。
  void setBaseUrlOverride(const String& url) { baseUrlOverride_ = url; }
  const String& baseUrlOverride() const { return baseUrlOverride_; }

  // 接続確認。使えるモデル数を count に入れる
  MuseResult listModels(int* count);

  // 16bit モノラル PCM を WAV にして送り、文字起こしの結果を受け取る
  MuseResult transcribe(const int16_t* pcm, size_t samples, uint32_t sampleRate, String& transcript);

  // Chat Completions のリクエスト JSON を送り、届いた文字列を onDelta へ順に渡す
  MuseResult chat(const String& requestJson, const std::function<void(const char*)>& onDelta);

  // 実行中の呼び出しをやめさせる（別のタスクから呼んでよい）
  void cancel() { cancelled_ = true; }
  void clearCancel() { cancelled_ = false; }

 private:
  bool open(const char* method, const char* path, const char* contentType, size_t contentLength,
            const char* accept, MuseResult& result);
  bool writeAll(const uint8_t* data, size_t length, MuseResult& result);
  bool readHead(uint32_t timeoutMs, MuseResult& result);
  void readError(MuseResult& result);
  void close();

  int fill(uint32_t timeoutMs);
  int rawByte(uint32_t timeoutMs);
  bool rawLine(String& line, uint32_t timeoutMs);
  int bodyByte(uint32_t timeoutMs);
  bool bodyLine(String& line, uint32_t timeoutMs);
  String bodyAll(size_t maxBytes, uint32_t timeoutMs);

  String apiKey_;
  String baseUrlOverride_;
  volatile bool cancelled_ = false;

  WiFiClientSecure secure_;
  WiFiClient plain_;
  WiFiClient* client_ = nullptr;

  uint8_t raw_[512];
  size_t rawPos_ = 0;
  size_t rawLen_ = 0;
  bool chunked_ = false;
  bool bodyEnded_ = false;
  int64_t bodyLeft_ = -1;  // Content-Length の残り（-1 = 切断まで）
  uint32_t chunkLeft_ = 0;
  bool isEventStream_ = false;
};
