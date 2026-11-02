#include "Recorder.h"

#include <M5Unified.h>
#include <math.h>

namespace {

constexpr size_t kBlockSamples = 512;          // 32ms
constexpr uint32_t kMaxSeconds = 20;           // 長すぎる録音は止める
constexpr size_t kCapacity = Recorder::kSampleRate * kMaxSeconds;

constexpr uint32_t kSkipBlocks = 4;            // マイク起動直後の過渡音（約130ms）
constexpr uint32_t kNoiseBlocks = 6;           // 周囲の音量を測る長さ（約190ms）
// 実機の静かな部屋で RMS 20〜30、サーボの動作音でも 160 程度
constexpr float kNoiseRatio = 3.5f;            // 周囲の音量の何倍で声とみなすか
constexpr float kMaxThreshold = 800.0f;        // 話しながら始めても判定できるように上限を置く
constexpr float kEndRatio = 0.6f;              // 話し終わりは少し低い音量で判定する
constexpr uint32_t kSpeechStartBlocks = 3;     // 約100ms 続いたら話し始めとする
constexpr uint32_t kSpeechEndBlocks = 28;      // 約900ms 静かなら話し終わりとする
constexpr uint32_t kMinSpeechBlocks = 8;       // 約250ms より短い音は声として扱わない
constexpr uint32_t kPadBlocks = 10;            // 前後に残す余白（約320ms）

constexpr float kTargetPeak = 24000.0f;
constexpr float kMaxGain = 8.0f;

constexpr uint8_t kMicTaskPriority = 3;

float blockRms(const int16_t* block) {
  int64_t sum = 0;
  int64_t squares = 0;
  for (size_t i = 0; i < kBlockSamples; ++i) {
    sum += block[i];
    squares += static_cast<int32_t>(block[i]) * block[i];
  }
  const float mean = static_cast<float>(sum) / kBlockSamples;
  const float variance = static_cast<float>(squares) / kBlockSamples - mean * mean;
  return variance > 0.0f ? sqrtf(variance) : 0.0f;
}

}  // namespace

bool Recorder::begin(LevelCallback onLevel) {
  onLevel_ = onLevel;
  buffer_ = static_cast<int16_t*>(
      heap_caps_malloc(kCapacity * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buffer_ == nullptr) return false;
  return xTaskCreatePinnedToCore(taskEntry, "recorder", 4096, this, kMicTaskPriority, &task_,
                                 APP_CPU_NUM) == pdPASS;
}

void Recorder::start(uint32_t silenceTimeoutMs, float minLevel) {
  if (task_ == nullptr || state_ == State::Recording) return;
  silenceTimeoutMs_ = silenceTimeoutMs;
  minLevel_ = minLevel;
  finishRequested_ = false;
  cancelRequested_ = false;
  trimStart_ = 0;
  trimCount_ = 0;
  state_ = State::Recording;
  xTaskNotifyGive(task_);
}

void Recorder::finish() {
  finishRequested_ = true;
}

void Recorder::cancel() {
  cancelRequested_ = true;
  while (state_ == State::Recording) delay(2);
  state_ = State::Idle;
}

void Recorder::taskEntry(void* self) {
  static_cast<Recorder*>(self)->run();
}

void Recorder::run() {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    record();
  }
}

void Recorder::analyzeBlock(const int16_t* block) {
  const float rms = blockRms(block);
  const uint32_t index = blocksSeen_++;

  if (onLevel_ != nullptr) {
    const float level = rms < 30.0f ? 0.0f : (log10f(rms) - 1.5f) / 1.8f * 100.0f;
    onLevel_(static_cast<uint8_t>(constrain(level, 0.0f, 100.0f)));
  }

  if (index < kSkipBlocks) return;
  if (index < kSkipBlocks + kNoiseBlocks) {
    // 一番静かなブロックを周囲の音量とする（動作音や話し始めで高く見積もらない）
    noiseFloor_ = index == kSkipBlocks ? rms : min(noiseFloor_, rms);
    return;
  }

  const float threshold = constrain(noiseFloor_ * kNoiseRatio, minLevel_, kMaxThreshold);
  if (rms > peakRms_) peakRms_ = rms;
  if (rms >= threshold) {
    ++loudBlocks_;
    quietBlocks_ = 0;
    if (!speechStarted_ && loudBlocks_ >= kSpeechStartBlocks) {
      speechStarted_ = true;
      speechStartBlock_ = index + 1 - kSpeechStartBlocks;
    }
    if (speechStarted_) speechEndBlock_ = index;
  } else if (rms >= threshold * kEndRatio && speechStarted_) {
    quietBlocks_ = 0;  // 声が小さくなっただけで、まだ話している
  } else {
    // 声の途切れで数え直さないよう、1 つずつ減らす
    if (!speechStarted_ && loudBlocks_ > 0) --loudBlocks_;
    ++quietBlocks_;
    if (speechStarted_ && quietBlocks_ >= kSpeechEndBlocks) stopByVad_ = true;
  }
  const uint32_t elapsedMs = index * kBlockSamples * 1000 / kSampleRate;
  if (!speechStarted_ && elapsedMs >= silenceTimeoutMs_) timedOut_ = true;
}

void Recorder::record() {
  blocksSeen_ = loudBlocks_ = quietBlocks_ = 0;
  speechStarted_ = stopByVad_ = timedOut_ = false;
  speechStartBlock_ = speechEndBlock_ = 0;
  noiseFloor_ = peakRms_ = 0.0f;
  gain_ = 1.0f;

  // M5.Mic は 2 ブロックまで予約できる。record() から戻った直後は、今渡したブロックと
  // その 1 つ前がまだ埋まっていないので、3 つ以上前のブロックだけを解析する
  size_t queued = 0;
  size_t analyzed = 0;
  bool failed = false;
  while (!finishRequested_ && !cancelRequested_ && !stopByVad_ && !timedOut_ &&
         queued + kBlockSamples <= kCapacity) {
    if (!M5.Mic.record(buffer_ + queued, kBlockSamples, kSampleRate)) {
      failed = true;
      break;
    }
    queued += kBlockSamples;
    while (analyzed + 3 * kBlockSamples <= queued) {
      analyzeBlock(buffer_ + analyzed);
      analyzed += kBlockSamples;
    }
  }
  while (M5.Mic.isRecording()) vTaskDelay(1);
  while (analyzed + kBlockSamples <= queued) {
    analyzeBlock(buffer_ + analyzed);
    analyzed += kBlockSamples;
  }

  Serial.printf("[mic] blocks=%u noise=%.0f peak=%.0f speech=%d (%u-%u) tap=%d vad=%d timeout=%d\n",
                static_cast<unsigned>(blocksSeen_), noiseFloor_, peakRms_, speechStarted_,
                static_cast<unsigned>(speechStartBlock_), static_cast<unsigned>(speechEndBlock_),
                finishRequested_, stopByVad_, timedOut_);

  if (cancelRequested_) {
    state_ = State::Idle;
    return;
  }
  if (failed) {
    state_ = State::Failed;
    return;
  }
  // タップで止めたときは、声と判定できなくても送る（小さな声でも聞いてほしいとき）
  const uint32_t voiced = speechStarted_ ? speechEndBlock_ - speechStartBlock_ + 1 : 0;
  if (!finishRequested_ && voiced < kMinSpeechBlocks) {
    state_ = State::NoSpeech;
    return;
  }
  if (finishRequested_ && !speechStarted_) {
    speechStartBlock_ = kSkipBlocks;
    speechEndBlock_ = analyzed / kBlockSamples - 1;
  }
  finalize(analyzed);
  state_ = State::Finished;
}

void Recorder::finalize(size_t recorded) {
  const size_t firstBlock = speechStartBlock_ > kPadBlocks + kSkipBlocks
                                ? speechStartBlock_ - kPadBlocks
                                : kSkipBlocks;
  const size_t totalBlocks = recorded / kBlockSamples;
  const size_t lastBlock = min(speechEndBlock_ + kPadBlocks, totalBlocks - 1);
  trimStart_ = firstBlock * kBlockSamples;
  trimCount_ = (lastBlock + 1 - firstBlock) * kBlockSamples;

  // 直流分を取り除いてから、小さな声でも聞き取れるように音量をそろえる
  int16_t* samples = buffer_ + trimStart_;
  int64_t sum = 0;
  for (size_t i = 0; i < trimCount_; ++i) sum += samples[i];
  const int32_t mean = static_cast<int32_t>(sum / static_cast<int64_t>(trimCount_));
  int32_t peak = 1;
  for (size_t i = 0; i < trimCount_; ++i) {
    const int32_t value = abs(samples[i] - mean);
    if (value > peak) peak = value;
  }
  gain_ = min(kTargetPeak / peak, kMaxGain);
  for (size_t i = 0; i < trimCount_; ++i) {
    const int32_t value = static_cast<int32_t>((samples[i] - mean) * gain_);
    samples[i] = static_cast<int16_t>(constrain(value, -32768, 32767));
  }
}
