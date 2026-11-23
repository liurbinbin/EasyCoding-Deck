#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <Arduino.h>
#include <driver/i2s_std.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>

// 单生产者（协议 loop）、单消费者（音频 task）；task 不调用协议或 I2C。
class AudioPlaybackStream {
 public:
  static constexpr size_t kBufferBytes = 16000 * 2 * 4;
  static constexpr size_t kInputSamples = 160;
  static constexpr size_t kOutputBytes = kInputSamples * 3 * 2 * sizeof(int16_t);
  static constexpr uint32_t kSampleRate = 48000;
  static constexpr uint32_t kDmaDescriptors = 8;
  static constexpr uint32_t kDmaFrames = 240;
  static constexpr uint32_t kDmaDurationMs = kDmaDescriptors * kDmaFrames * 1000 / kSampleRate;
  enum class State : uint8_t { Idle, Buffering, Playing, Completed, Failed };

  bool configure();
  void close();
  bool start();
  void stop();
  bool enqueue(const uint8_t* pcm, size_t bytes);
  void end() { ended_.store(true); }
  size_t buffered() const;
  size_t written() const { return written_.load(); }
  uint32_t underruns() const { return underruns_.load(); }
  esp_err_t error() const { return error_.load(); }
  State state() const { return state_.load(); }
  bool clockInfo(i2s_chan_info_t& info) const;
  size_t writeCue(const uint8_t* data, size_t bytes);

 private:
  static void taskEntry(void* context);
  void run();
  bool allocate();
  i2s_chan_handle_t tx_ = nullptr;
  TaskHandle_t task_ = nullptr;
  SemaphoreHandle_t idle_ = nullptr;
  StreamBufferHandle_t stream_ = nullptr;
  StaticStreamBuffer_t stream_control_{};
  uint8_t* storage_ = nullptr;
  uint8_t* output_[2] = {nullptr, nullptr};
  bool worker_started_ = false;  // 仅由协议 loop 访问。
  std::atomic<bool> cancelled_{true};
  std::atomic<bool> ended_{false};
  std::atomic<State> state_{State::Idle};
  std::atomic<size_t> written_{0};
  std::atomic<uint32_t> underruns_{0};
  std::atomic<esp_err_t> error_{ESP_OK};
};
