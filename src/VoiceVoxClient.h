#pragma once

#include <Arduino.h>

// TTSエンジンの種別
enum class TtsEngineType {
  VoiceVoxCompatible, // VoiceVox互換API（2段階: audio_query → synthesis）
  SimpleWav,          // シンプルなWAV返却API（テキストPOST → WAVレスポンス）
};

// TTS接続設定。speak()呼び出し時に渡す。
struct TtsConfig {
  String host;                                              // TTSサーバのIPアドレスまたはホスト名
  uint16_t port = 50021;                                   // TTSサーバのポート番号
  String speaker = "3";                                    // 話者ID（VoiceVox: ずんだもんノーマル=3）
  TtsEngineType engineType = TtsEngineType::VoiceVoxCompatible; // 使用するエンジン種別
};

// 口パク同期コールバック: WAV再生中に音量レベル(0〜100)を渡す
using LipSyncCallback = void (*)(int level);

// サービスコールバック: WAV受信/再生中にメインループの処理を継続するために呼ぶ
using ServiceCallback = void (*)();

// HTTPでTTSサーバに接続し、WAVデータをストリーミング再生するクライアント。
//
// VoiceVox互換モードの動作:
//   1. POST /audio_query?text=...&speaker=... → 音声合成パラメータJSON取得
//   2. POST /synthesis?speaker=... (Body: JSON) → WAVストリームを受け取り再生
//
// SimpleWavモードの動作:
//   1. POST /synthesis (Body: テキスト) → WAVストリームを受け取り再生
//
// メモリ効率: 3バッファ×2KBでストリーミング受信しながら同時再生する。
// 大きなWAVファイルでもRAMを消費しない。
class TtsClient {
 public:
  // コールバックを登録する。
  //   lipSync: 音声振幅(0〜100)を口パク制御に渡す
  //   service: 通信中にメインループ処理（Webサーバ応答など）を継続するために呼ぶ
  void setCallbacks(LipSyncCallback lipSync, ServiceCallback service);

  // テキストを音声合成してスピーカで再生する。ブロッキング処理。
  // 戻り値: 成功=true、失敗=false（lastError()でエラー内容を取得できる）
  bool speak(const TtsConfig& config, const String& text);

  // 直近のエラーメッセージを返す（成功時は空文字列）
  const String& lastError() const;

  // 再生中の speak() をできるだけ早く終わらせる（service コールバックから呼んでよい）
  void abort() { abort_ = true; }

 private:
  LipSyncCallback lipSync_ = nullptr; // 口パクコールバック
  ServiceCallback service_ = nullptr; // サービスコールバック
  String lastError_;                  // 最後のエラーメッセージ
  volatile bool abort_ = false;       // abort() が呼ばれた

  // TTSサーバのエンドポイントURLを生成する（http://host:port/path）
  String endpoint(const TtsConfig& config, const char* path) const;

  // 文字列をURLエンコードする（RFC 3986準拠、日本語テキストに対応）
  String urlEncode(const String& value) const;

  // VoiceVox互換モードでの音声合成・再生（2段階HTTPリクエスト）
  bool speakVoiceVoxCompatible(const TtsConfig& config, const String& text);

  // SimpleWavモードでの音声合成・再生（1段階HTTPリクエスト）
  bool speakSimpleWav(const TtsConfig& config, const String& text);

  // HTTPクライアントからWAVストリームを受信して再生する
  bool playWavStream(class HTTPClient& http);

  // ストリームから指定バイト数を正確に読み取る（タイムアウト付き）
  bool readExact(class WiFiClient& stream, uint8_t* destination, size_t length,
                 uint32_t timeoutMs);

  // ストリームの指定バイト数を読み捨てる（WAVチャンクのスキップに使用）
  bool skipBytes(class WiFiClient& stream, size_t length, uint32_t timeoutMs);

  // serviceコールバックを安全に呼ぶ（nullチェック付き）
  void service();
};

// 後方互換性のためのエイリアス
using VoiceVoxClient = TtsClient;
