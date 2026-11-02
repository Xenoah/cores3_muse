#include "MuseClient.h"

#include <ArduinoJson.h>

#include "MuseRootCa.h"

namespace {

constexpr int32_t kConnectTimeoutMs = 15000;
constexpr uint32_t kHeadTimeoutMs = 60000;   // 考えている間はヘッダが遅れることがある
constexpr uint32_t kStreamTimeoutMs = 60000;
constexpr uint32_t kWriteStallMs = 15000;
constexpr size_t kWriteChunk = 4096;

constexpr char kTranscribeModel[] = "muse-voice-transcribe-1.0";

// 16bit モノラル PCM の WAV ヘッダ
void writeWavHeader(uint8_t* h, uint32_t dataBytes, uint32_t sampleRate) {
  auto u32 = [](uint8_t* p, uint32_t v) {
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
  };
  auto u16 = [](uint8_t* p, uint16_t v) {
    p[0] = v;
    p[1] = v >> 8;
  };
  memcpy(h, "RIFF", 4);
  u32(h + 4, 36 + dataBytes);
  memcpy(h + 8, "WAVEfmt ", 8);
  u32(h + 16, 16);
  u16(h + 20, 1);  // PCM
  u16(h + 22, 1);  // モノラル
  u32(h + 24, sampleRate);
  u32(h + 28, sampleRate * 2);
  u16(h + 32, 2);
  u16(h + 34, 16);
  memcpy(h + 36, "data", 4);
  u32(h + 40, dataBytes);
}

}  // namespace

// --- 接続と送信 -----------------------------------------------------------

bool MuseClient::open(const char* method, const char* path, const char* contentType,
                      size_t contentLength, const char* accept, MuseResult& result) {
  const bool isDefault = baseUrlOverride_.isEmpty();
  const String base = isDefault ? String(kDefaultBaseUrl) : baseUrlOverride_;
  const bool tls = base.startsWith("https://");
  const int hostStart = base.indexOf("://") + 3;
  int pathStart = base.indexOf('/', hostStart);
  if (pathStart < 0) pathStart = base.length();
  String host = base.substring(hostStart, pathStart);
  const String prefix = base.substring(pathStart);
  uint16_t port = tls ? 443 : 80;
  const int colon = host.indexOf(':');
  if (colon >= 0) {
    port = host.substring(colon + 1).toInt();
    host = host.substring(0, colon);
  }

  rawPos_ = rawLen_ = 0;
  chunked_ = false;
  bodyEnded_ = false;
  bodyLeft_ = -1;
  chunkLeft_ = 0;
  isEventStream_ = false;

  bool connected;
  if (tls) {
    secure_.setCACert(kMuseRootCa);
    secure_.setHandshakeTimeout(20);
    client_ = &secure_;
    connected = secure_.connect(host.c_str(), port, kConnectTimeoutMs);
  } else {
    client_ = &plain_;
    connected = plain_.connect(host.c_str(), port, kConnectTimeoutMs);
  }
  if (!connected) {
    result.code = "connect_failed";
    if (tls) {
      char detail[96] = "";
      secure_.lastError(detail, sizeof(detail));
      result.message = detail;
    }
    close();
    return false;
  }

  // 小さく分けて送るので、遅延 ACK と Nagle で待たされないようにする
  client_->setNoDelay(true);

  // 本物のキーは既定の接続先にだけ送る
  const String key = isDefault ? apiKey_ : String("test-key");
  String head;
  head.reserve(320);
  head += method;
  head += ' ';
  head += prefix;
  head += path;
  head += " HTTP/1.1\r\nHost: ";
  head += host;
  head += "\r\nUser-Agent: stackchan-muse\r\nConnection: close\r\nAccept: ";
  head += accept;
  head += "\r\n";
  if (!key.isEmpty()) {
    head += "Authorization: Bearer ";
    head += key;
    head += "\r\n";
  }
  if (contentType != nullptr) {
    head += "Content-Type: ";
    head += contentType;
    head += "\r\nContent-Length: ";
    head += String(static_cast<unsigned>(contentLength));
    head += "\r\n";
  }
  head += "\r\n";
  return writeAll(reinterpret_cast<const uint8_t*>(head.c_str()), head.length(), result);
}

bool MuseClient::writeAll(const uint8_t* data, size_t length, MuseResult& result) {
  uint32_t lastProgress = millis();
  while (length > 0) {
    if (cancelled_) {
      result.cancelled = true;
      close();
      return false;
    }
    const size_t written = client_->write(data, min(length, kWriteChunk));
    if (written == 0) {
      if (!client_->connected() || millis() - lastProgress > kWriteStallMs) {
        result.code = "write_failed";
        close();
        return false;
      }
      delay(5);
      continue;
    }
    data += written;
    length -= written;
    lastProgress = millis();
  }
  return true;
}

void MuseClient::close() {
  if (client_ != nullptr) client_->stop();
  client_ = nullptr;
}

// --- 受信 -----------------------------------------------------------------

// 戻り値: 読めたバイト数、-1 = 切断、-2 = 取り消し、-3 = 時間切れ
int MuseClient::fill(uint32_t timeoutMs) {
  const uint32_t started = millis();
  for (;;) {
    if (cancelled_) return -2;
    const int available = client_->available();
    if (available > 0) {
      const int n = client_->read(raw_, min<size_t>(available, sizeof(raw_)));
      if (n > 0) {
        rawPos_ = 0;
        rawLen_ = n;
        return n;
      }
    }
    if (!client_->connected()) return -1;
    if (millis() - started > timeoutMs) return -3;
    delay(2);
  }
}

int MuseClient::rawByte(uint32_t timeoutMs) {
  if (rawPos_ >= rawLen_) {
    const int n = fill(timeoutMs);
    if (n <= 0) return n;
  }
  return raw_[rawPos_++];
}

bool MuseClient::rawLine(String& line, uint32_t timeoutMs) {
  line = "";
  for (;;) {
    const int c = rawByte(timeoutMs);
    if (c < 0) return false;
    if (c == '\n') break;
    if (c != '\r') line += static_cast<char>(c);
  }
  return true;
}

bool MuseClient::readHead(uint32_t timeoutMs, MuseResult& result) {
  String line;
  if (!rawLine(line, timeoutMs)) {
    result.cancelled = cancelled_;
    result.code = "no_response";
    close();
    return false;
  }
  const int space = line.indexOf(' ');
  result.status = space > 0 ? line.substring(space + 1, space + 4).toInt() : 0;
  while (rawLine(line, timeoutMs) && !line.isEmpty()) {
    const int colon = line.indexOf(':');
    if (colon < 0) continue;
    String name = line.substring(0, colon);
    name.toLowerCase();
    String value = line.substring(colon + 1);
    value.trim();
    if (name == "transfer-encoding") {
      value.toLowerCase();
      chunked_ = value.indexOf("chunked") >= 0;
    } else if (name == "content-length") {
      bodyLeft_ = value.toInt();
    } else if (name == "content-type") {
      isEventStream_ = value.indexOf("text/event-stream") >= 0;
    }
  }
  return true;
}

int MuseClient::bodyByte(uint32_t timeoutMs) {
  if (bodyEnded_) return -1;
  if (chunked_) {
    if (chunkLeft_ == 0) {
      String size;
      if (!rawLine(size, timeoutMs)) return -1;
      if (size.isEmpty() && !rawLine(size, timeoutMs)) return -1;  // 前のチャンクの CRLF
      chunkLeft_ = strtoul(size.c_str(), nullptr, 16);
      if (chunkLeft_ == 0) {
        bodyEnded_ = true;
        return -1;
      }
    }
    const int c = rawByte(timeoutMs);
    if (c >= 0) --chunkLeft_;
    return c;
  }
  if (bodyLeft_ == 0) {
    bodyEnded_ = true;
    return -1;
  }
  const int c = rawByte(timeoutMs);
  if (c >= 0 && bodyLeft_ > 0) --bodyLeft_;
  return c;
}

bool MuseClient::bodyLine(String& line, uint32_t timeoutMs) {
  line = "";
  for (;;) {
    const int c = bodyByte(timeoutMs);
    if (c < 0) return !line.isEmpty();
    if (c == '\n') return true;
    if (c != '\r') line += static_cast<char>(c);
  }
}

String MuseClient::bodyAll(size_t maxBytes, uint32_t timeoutMs) {
  String body;
  for (;;) {
    const int c = bodyByte(timeoutMs);
    if (c < 0) break;
    if (body.length() < maxBytes) body += static_cast<char>(c);
  }
  return body;
}

void MuseClient::readError(MuseResult& result) {
  const String body = bodyAll(2048, 5000);
  JsonDocument doc;
  if (!deserializeJson(doc, body)) {
    result.code = doc["error"]["code"] | "";
    result.message = doc["error"]["message"] | "";
  }
  if (result.message.isEmpty()) result.message = body.substring(0, 160);
}

// --- API ------------------------------------------------------------------

MuseResult MuseClient::listModels(int* count) {
  MuseResult result;
  if (!open("GET", "/models", nullptr, 0, "application/json", result)) return result;
  if (!readHead(30000, result)) return result;
  if (result.status != 200) {
    readError(result);
    close();
    return result;
  }
  const String body = bodyAll(16384, 10000);
  close();
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    result.code = "bad_json";
    return result;
  }
  if (count != nullptr) *count = doc["data"].as<JsonArrayConst>().size();
  result.ok = true;
  return result;
}

MuseResult MuseClient::transcribe(const int16_t* pcm, size_t samples, uint32_t sampleRate,
                                  String& transcript) {
  MuseResult result;
  transcript = "";
  const String boundary = "----stackchanmuse" + String(esp_random(), HEX);

  // 日本語と英語を想定する（languageBias はヒントで、他の言語も検出される）
  JsonDocument settings;
  settings["mode"] = "PUSH_TO_TALK";
  settings["model"] = kTranscribeModel;
  settings["audioEncoding"] = "WAV";
  JsonArray bias = settings["languageBias"].to<JsonArray>();
  bias.add("Japanese");
  bias.add("English");
  String settingsJson;
  serializeJson(settings, settingsJson);

  String before = "--" + boundary +
                  "\r\nContent-Disposition: form-data; name=\"request\""
                  "\r\nContent-Type: application/json\r\n\r\n" +
                  settingsJson + "\r\n--" + boundary +
                  "\r\nContent-Disposition: form-data; name=\"audio\"; filename=\"speech.wav\""
                  "\r\nContent-Type: audio/wav\r\n\r\n";
  const String after = "\r\n--" + boundary + "--\r\n";

  const uint32_t pcmBytes = samples * sizeof(int16_t);
  uint8_t wav[44];
  writeWavHeader(wav, pcmBytes, sampleRate);

  const String contentType = "multipart/form-data; boundary=" + boundary;
  const size_t total = before.length() + sizeof(wav) + pcmBytes + after.length();
  if (!open("POST", "/asr/transcribe", contentType.c_str(), total, "application/json", result)) {
    return result;
  }
  if (!writeAll(reinterpret_cast<const uint8_t*>(before.c_str()), before.length(), result) ||
      !writeAll(wav, sizeof(wav), result) ||
      !writeAll(reinterpret_cast<const uint8_t*>(pcm), pcmBytes, result) ||
      !writeAll(reinterpret_cast<const uint8_t*>(after.c_str()), after.length(), result)) {
    return result;
  }
  if (!readHead(kHeadTimeoutMs, result)) return result;
  if (result.status != 200) {
    readError(result);
    close();
    return result;
  }
  const String body = bodyAll(16384, 10000);
  close();
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    result.code = "bad_json";
    return result;
  }
  transcript = doc["transcript"] | "";
  transcript.trim();
  result.ok = true;
  return result;
}

MuseResult MuseClient::chat(const String& requestJson,
                            const std::function<void(const char*)>& onDelta) {
  MuseResult result;
  if (!open("POST", "/chat/completions", "application/json", requestJson.length(),
            "text/event-stream", result)) {
    return result;
  }
  if (!writeAll(reinterpret_cast<const uint8_t*>(requestJson.c_str()), requestJson.length(),
                result)) {
    return result;
  }
  if (!readHead(kHeadTimeoutMs, result)) return result;
  if (result.status != 200) {
    readError(result);
    close();
    return result;
  }

  JsonDocument filter;
  filter["choices"][0]["delta"]["content"] = true;
  filter["choices"][0]["message"]["content"] = true;
  filter["error"] = true;

  // ストリーミングでなく JSON 全体が返ってきても受け取る
  if (!isEventStream_) {
    const String body = bodyAll(32768, kStreamTimeoutMs);
    close();
    JsonDocument doc;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
      result.code = "bad_json";
      return result;
    }
    const char* text = doc["choices"][0]["message"]["content"];
    if (text != nullptr) onDelta(text);
    result.ok = text != nullptr;
    return result;
  }

  String line;
  bool done = false;
  while (!done && bodyLine(line, kStreamTimeoutMs)) {
    if (!line.startsWith("data:")) continue;
    const char* payload = line.c_str() + 5;
    while (*payload == ' ') ++payload;
    if (strcmp(payload, "[DONE]") == 0) {
      done = true;
      break;
    }
    JsonDocument doc;
    if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) continue;
    if (doc["error"].is<JsonObjectConst>()) {
      result.code = doc["error"]["code"] | "stream_error";
      result.message = doc["error"]["message"] | "";
      close();
      return result;
    }
    const char* delta = doc["choices"][0]["delta"]["content"];
    if (delta != nullptr && delta[0] != '\0') onDelta(delta);
  }
  result.cancelled = cancelled_;
  close();
  // [DONE] が来る前に切れたら途中までの文で終わる。呼び出し側が失敗を知れるようにする
  result.ok = done && !result.cancelled;
  if (!done && !result.cancelled) result.code = "stream_interrupted";
  return result;
}
