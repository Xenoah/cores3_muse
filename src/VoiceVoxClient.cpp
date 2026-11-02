#include "VoiceVoxClient.h"

#include <HTTPClient.h>
#include <M5Unified.h>
#include <WiFiClient.h>

namespace {

// ストリーミング再生用のバッファ設定。
// 3バッファを使いまわすことで、受信しながら前バッファを再生できる。
constexpr size_t kAudioBufferCount = 3;
constexpr size_t kAudioBufferSize = 2048; // バッファ1個あたり2KB

// ネットワーク操作のタイムアウト（30秒）
constexpr uint32_t kNetworkTimeoutMs = 30000;
// TCP 接続のタイムアウト。LAN 内なら数秒で十分で、サーバー停止時に長く固まらないよう短くする
constexpr uint32_t kConnectTimeoutMs = 4000;

// 4バイトアライメントで確保（M5Speakerのplayrawが4バイトアライメントを要求）
alignas(4) uint8_t audioBuffers[kAudioBufferCount][kAudioBufferSize];

// リトルエンディアン16ビット整数を読み取る（WAFヘッダ解析用）
uint16_t readLe16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) |
         (static_cast<uint16_t>(data[1]) << 8);
}

// リトルエンディアン32ビット整数を読み取る（WAFヘッダ解析用）
uint32_t readLe32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

}  // namespace

void TtsClient::setCallbacks(LipSyncCallback lipSync,
                             ServiceCallback serviceCallback) {
  lipSync_ = lipSync;
  service_ = serviceCallback;
}

// テキストを音声合成してスピーカで再生する。
// WiFi未接続の場合はすぐにfalseを返す。
bool TtsClient::speak(const TtsConfig& config, const String& text) {
  lastError_ = "";
  abort_ = false;
  if (WiFi.status() != WL_CONNECTED) {
    lastError_ = "Wi-Fi is not connected";
    return false;
  }

  if (config.engineType == TtsEngineType::SimpleWav) {
    return speakSimpleWav(config, text);
  }
  return speakVoiceVoxCompatible(config, text);
}

// VoiceVox互換API（2段階）での音声合成・再生。
//   ステップ1: audio_query でテキストを解析しパラメータJSONを取得
//   ステップ2: synthesis でJSONを元にWAVを生成してストリーミング再生
bool TtsClient::speakVoiceVoxCompatible(
    const TtsConfig& config, const String& text) {
  const String speaker = urlEncode(config.speaker);
  const String queryUrl =
      endpoint(config, "/audio_query") + "?speaker=" + speaker +
      "&text=" + urlEncode(text);

  // ステップ1: audio_query リクエスト
  HTTPClient queryHttp;
  queryHttp.setConnectTimeout(kConnectTimeoutMs);
  queryHttp.setTimeout(kNetworkTimeoutMs);
  if (!queryHttp.begin(queryUrl)) {
    lastError_ = "audio_query begin failed";
    return false;
  }

  const int queryStatus = queryHttp.POST(""); // GETではなくPOSTが必要
  if (queryStatus != HTTP_CODE_OK) {
    lastError_ = "audio_query HTTP " + String(queryStatus);
    queryHttp.end();
    return false;
  }

  String audioQuery = queryHttp.getString(); // 音声合成パラメータJSON
  queryHttp.end();
  if (audioQuery.isEmpty()) {
    lastError_ = "audio_query returned empty JSON";
    return false;
  }

  // ステップ2: synthesis リクエスト（JSONをBodyとして送信）
  const String synthesisUrl =
      endpoint(config, "/synthesis") + "?speaker=" + speaker;
  HTTPClient synthesisHttp;
  synthesisHttp.setConnectTimeout(kConnectTimeoutMs);
  synthesisHttp.setTimeout(kNetworkTimeoutMs);
  if (!synthesisHttp.begin(synthesisUrl)) {
    lastError_ = "synthesis begin failed";
    return false;
  }

  synthesisHttp.addHeader("Content-Type", "application/json");
  const int synthesisStatus = synthesisHttp.POST(
      reinterpret_cast<uint8_t*>(
          const_cast<char*>(audioQuery.c_str())),
      audioQuery.length());
  audioQuery.clear(); // 大きなJSONをすぐに解放してRAMを節約

  if (synthesisStatus != HTTP_CODE_OK) {
    lastError_ = "synthesis HTTP " + String(synthesisStatus);
    synthesisHttp.end();
    return false;
  }

  const bool played = playWavStream(synthesisHttp);
  synthesisHttp.end();
  return played;
}

// SimpleWav API（1段階）での音声合成・再生。
// テキストをBodyとして直接POSTし、WAVレスポンスをストリーミング再生する。
bool TtsClient::speakSimpleWav(
    const TtsConfig& config, const String& text) {
  HTTPClient synthesisHttp;
  synthesisHttp.setConnectTimeout(kConnectTimeoutMs);
  synthesisHttp.setTimeout(kNetworkTimeoutMs);
  if (!synthesisHttp.begin(endpoint(config, "/synthesis"))) {
    lastError_ = "simple_wav synthesis begin failed";
    return false;
  }

  synthesisHttp.addHeader("Content-Type", "text/plain; charset=utf-8");
  synthesisHttp.addHeader("Accept", "audio/wav");
  const int synthesisStatus = synthesisHttp.POST(
      reinterpret_cast<uint8_t*>(const_cast<char*>(text.c_str())),
      text.length());

  if (synthesisStatus != HTTP_CODE_OK) {
    lastError_ = "simple_wav synthesis HTTP " + String(synthesisStatus);
    synthesisHttp.end();
    return false;
  }

  const bool played = playWavStream(synthesisHttp);
  synthesisHttp.end();
  return played;
}

const String& TtsClient::lastError() const {
  return lastError_;
}

// TTSサーバのエンドポイントURLを組み立てる
String TtsClient::endpoint(const TtsConfig& config,
                           const char* path) const {
  return "http://" + config.host + ":" + String(config.port) + path;
}

// 文字列をURLエンコードする。RFC 3986の非予約文字はそのまま、
// それ以外のバイト（日本語マルチバイト含む）は %XX 形式に変換する。
String TtsClient::urlEncode(const String& value) const {
  static const char hex[] = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(value.length() * 3); // 最大3倍（全文字がエンコードされる場合）

  const uint8_t* bytes =
      reinterpret_cast<const uint8_t*>(value.c_str());
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t c = bytes[i];
    // RFC 3986の非予約文字はエンコード不要
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~') {
      encoded += static_cast<char>(c);
    } else {
      // %XX 形式にエンコード（上位4ビット・下位4ビットをそれぞれ16進数に）
      encoded += '%';
      encoded += hex[c >> 4];
      encoded += hex[c & 0x0F];
    }
  }
  return encoded;
}

// HTTPレスポンスのWAVストリームを受信しながらリアルタイムで再生する。
//
// WAV解析フロー:
//   1. RIFF/WAVEヘッダ（12バイト）を読んで形式を確認
//   2. チャンクを順番に読む:
//      - "fmt " チャンク → フォーマット情報（サンプルレート・ビット深度等）を取得
//      - "data" チャンク → PCMデータをバッファに読み込んで再生
//      - その他のチャンク → スキップ
//   3. dataチャンクは3バッファを循環使用し、受信しながら前バッファを再生する
bool TtsClient::playWavStream(HTTPClient& http) {
  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr) {
    lastError_ = "synthesis stream unavailable";
    return false;
  }

  // WAFヘッダ確認: 先頭4バイト="RIFF"、8〜11バイト目="WAVE"
  uint8_t riffHeader[12];
  if (!readExact(*stream, riffHeader, sizeof(riffHeader),
                 kNetworkTimeoutMs) ||
      memcmp(riffHeader, "RIFF", 4) != 0 ||
      memcmp(riffHeader + 8, "WAVE", 4) != 0) {
    lastError_ = "invalid WAV RIFF header";
    return false;
  }

  uint16_t audioFormat = 0;    // 1=PCM（整数）のみサポート
  uint16_t channels = 0;       // 1=モノラル, 2=ステレオ
  uint16_t bitsPerSample = 0;  // 16ビットのみサポート
  uint32_t sampleRate = 0;     // サンプルレート（Hz）
  uint32_t dataLength = 0;     // PCMデータの総バイト数

  // チャンクを順番に読み、"data"チャンクが見つかるまでループ
  while (dataLength == 0) {
    uint8_t chunkHeader[8]; // チャンクID(4バイト) + サイズ(4バイト)
    if (!readExact(*stream, chunkHeader, sizeof(chunkHeader),
                   kNetworkTimeoutMs)) {
      lastError_ = "WAV chunk header timeout";
      return false;
    }

    const uint32_t chunkLength = readLe32(chunkHeader + 4);
    if (memcmp(chunkHeader, "fmt ", 4) == 0) {
      // fmtチャンク: フォーマット情報を読み取る（最低16バイト必要）
      if (chunkLength < 16) {
        lastError_ = "invalid WAV fmt chunk";
        return false;
      }

      uint8_t format[16];
      if (!readExact(*stream, format, sizeof(format), kNetworkTimeoutMs)) {
        lastError_ = "WAV fmt timeout";
        return false;
      }

      audioFormat = readLe16(format);      // オーディオフォーマット
      channels = readLe16(format + 2);     // チャンネル数
      sampleRate = readLe32(format + 4);   // サンプルレート(Hz)
      bitsPerSample = readLe16(format + 14); // 量子化ビット数

      // fmtチャンクが16バイトより大きい場合（拡張ヘッダ）は残りをスキップ
      if (chunkLength > sizeof(format) &&
          !skipBytes(*stream, chunkLength - sizeof(format),
                     kNetworkTimeoutMs)) {
        lastError_ = "WAV fmt extension timeout";
        return false;
      }
    } else if (memcmp(chunkHeader, "data", 4) == 0) {
      dataLength = chunkLength; // dataチャンク発見: ループを抜ける
    } else if (!skipBytes(*stream, chunkLength, kNetworkTimeoutMs)) {
      // 未知のチャンク（LIST, INFO等）はスキップする
      lastError_ = "WAV chunk skip timeout";
      return false;
    }

    // WAFチャンクはWORDアライメント（2バイト境界）なので奇数サイズの場合パディングを読み飛ばす
    if ((chunkLength & 1) != 0 && dataLength == 0 &&
        !skipBytes(*stream, 1, kNetworkTimeoutMs)) {
      lastError_ = "WAV padding timeout";
      return false;
    }
  }

  // このプロジェクトでサポートする形式: 16ビットPCM、モノラルorステレオのみ
  if (audioFormat != 1 || bitsPerSample != 16 ||
      (channels != 1 && channels != 2) || sampleRate == 0) {
    lastError_ = "unsupported WAV format";
    return false;
  }

  // PCMデータをバッファに分割して受信・再生するメインループ
  size_t bufferIndex = 0;
  uint32_t remaining = dataLength; // 残りの未受信バイト数
  while (remaining > 0 && !abort_) {
    size_t readLength = min<size_t>(remaining, kAudioBufferSize);
    readLength &= ~static_cast<size_t>(1); // 2バイト境界にアライメント（16ビットサンプルのため）
    if (readLength == 0 ||
        !readExact(*stream, audioBuffers[bufferIndex], readLength,
                   kNetworkTimeoutMs)) {
      lastError_ = "WAV audio stream timeout";
      return false;
    }

    // 16ビット符号付き整数サンプルとして解釈する
    const int16_t* samples =
        reinterpret_cast<const int16_t*>(audioBuffers[bufferIndex]);
    const size_t sampleCount = readLength / sizeof(int16_t);

    // 口パク同期: バッファ内のサンプルの平均振幅を計算し、0〜100にスケールする。
    // ステレオの場合はLチャンネルのみ（偶数インデックス）を使用する。
    // 閾値 180〜5200 は実験値（環境音ノイズを除外しつつ音声を検出）。
    if (lipSync_ != nullptr) {
      uint64_t amplitude = 0;
      for (size_t i = 0; i < sampleCount; i += channels) {
        const int32_t sample = samples[i];
        amplitude += sample < 0 ? -sample : sample; // 絶対値の累積
      }
      const size_t frameCount = max<size_t>(1, sampleCount / channels);
      const int average = static_cast<int>(amplitude / frameCount);
      const int level = constrain(
          (average - 180) * 100 / (5200 - 180), 0, 100);
      lipSync_(level);
    }

    // M5Speakerでバッファを非ブロッキング再生（次のバッファを受信しながら再生）
    M5.Speaker.playRaw(samples, sampleCount, sampleRate, channels == 2, 1,
                       0, false);
    remaining -= readLength;
    bufferIndex = (bufferIndex + 1) % kAudioBufferCount; // 次のバッファに切り替え
    service(); // メインループ処理を継続（Webサーバ応答など）
  }

  if (abort_) M5.Speaker.stop(0);

  // スピーカの再生キューが空になるまで待機する
  while (M5.Speaker.isPlaying(0)) {
    service();
    delay(5);
  }

  // 再生完了後に口を閉じる
  if (lipSync_ != nullptr) {
    lipSync_(0);
  }
  return true;
}

// ストリームから指定バイト数を正確に読み取る。
// 一度に読み取れる量はTCPバッファ次第なので、ループで少しずつ読む。
// タイムアウト内に進捗がなければ接続切れと判断してfalseを返す。
bool TtsClient::readExact(WiFiClient& stream, uint8_t* destination,
                          size_t length, uint32_t timeoutMs) {
  size_t offset = 0;
  uint32_t lastProgress = millis(); // 最後にデータを受信した時刻

  while (offset < length) {
    const int available = stream.available();
    if (available > 0) {
      const size_t count =
          min<size_t>(length - offset, static_cast<size_t>(available));
      const int received = stream.read(destination + offset, count);
      if (received > 0) {
        offset += received;
        lastProgress = millis(); // 受信があれば進捗タイマーをリセット
      }
    } else if (!stream.connected()) {
      return false; // 接続が切れた
    } else if (millis() - lastProgress >= timeoutMs) {
      return false; // タイムアウト: 進捗なしで指定時間経過
    }

    service(); // 待機中もメインループ処理を継続する
    delay(1);
  }
  return true;
}

// ストリームの指定バイト数を読み捨てる。
// 64バイトのスクラッチバッファを使い、大量のスキップも分割処理する。
bool TtsClient::skipBytes(WiFiClient& stream, size_t length,
                          uint32_t timeoutMs) {
  uint8_t scratch[64];
  while (length > 0) {
    const size_t count = min(length, sizeof(scratch));
    if (!readExact(stream, scratch, count, timeoutMs)) {
      return false;
    }
    length -= count;
  }
  return true;
}

// serviceコールバックをnullチェック付きで呼び出す
void TtsClient::service() {
  if (service_ != nullptr) {
    service_();
  }
}
