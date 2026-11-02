#pragma once

#include <Arduino.h>

// マイクの録音。話し始めと話し終わりを音量で見分けて、自動で止まる。
// M5.Mic は専用のタスクで読み続ける。CoreS3 のマイクとスピーカーは I2S を共有するので、
// 呼び出す側で M5.Speaker を止めてから M5.Mic.begin() し、start() を呼ぶ。
class Recorder {
 public:
  static constexpr uint32_t kSampleRate = 16000;

  enum class State : uint8_t {
    Idle,
    Recording,
    Finished,  // 話し終わった（samples() で取り出せる）
    NoSpeech,  // 誰も話さなかった
    Failed,    // 録音できなかった
  };

  using LevelCallback = void (*)(uint8_t level);

  bool begin(LevelCallback onLevel);

  // silenceTimeoutMs の間に話し始めなければ NoSpeech で止まる。
  // minLevel より小さな音（RMS）は声として扱わない
  void start(uint32_t silenceTimeoutMs, float minLevel);
  void finish();  // 今の録音をここで終える（タップで止めるとき）
  void cancel();  // 録音を捨てる

  State state() const { return state_; }
  bool isRunning() const { return state_ == State::Recording; }

  // 無音を落として音量を整えた音声。次の start() まで有効
  const int16_t* samples() const { return buffer_ + trimStart_; }
  size_t sampleCount() const { return trimCount_; }

  // 調整用の情報（直近の録音）
  float noiseFloor() const { return noiseFloor_; }
  float peakRms() const { return peakRms_; }
  float gain() const { return gain_; }

 private:
  static void taskEntry(void* self);
  void run();
  void record();
  void analyzeBlock(const int16_t* block);
  void finalize(size_t recorded);

  int16_t* buffer_ = nullptr;
  TaskHandle_t task_ = nullptr;
  LevelCallback onLevel_ = nullptr;
  volatile State state_ = State::Idle;
  volatile bool finishRequested_ = false;
  volatile bool cancelRequested_ = false;
  uint32_t silenceTimeoutMs_ = 0;
  float minLevel_ = 0.0f;

  size_t trimStart_ = 0;
  size_t trimCount_ = 0;

  // 話し始め・話し終わりの判定
  uint32_t blocksSeen_ = 0;
  uint32_t loudBlocks_ = 0;
  uint32_t quietBlocks_ = 0;
  bool speechStarted_ = false;
  bool stopByVad_ = false;
  bool timedOut_ = false;
  size_t speechStartBlock_ = 0;
  size_t speechEndBlock_ = 0;
  float noiseFloor_ = 0.0f;
  float peakRms_ = 0.0f;
  float gain_ = 1.0f;
};
