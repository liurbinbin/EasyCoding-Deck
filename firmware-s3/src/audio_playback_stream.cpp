#include "audio_playback_stream.h"

#include <cstring>
#include <esp_heap_caps.h>
#include "pin_config.h"

namespace {
constexpr size_t kStartBytes = 16000 * 2 / 5;  // 200 ms 网络抖动预缓冲。
constexpr size_t kResumeBytes = 16000 * 2 / 10;
constexpr uint32_t kWriteTimeoutMs = 20;
constexpr uint32_t kDrainMs = AudioPlaybackStream::kDmaDurationMs + 10;
static_assert(AudioPlaybackStream::kDmaFrames * 2 * sizeof(int16_t) <= 4092,
              "one DMA descriptor must fit the ESP32 DMA byte limit");
static_assert(AudioPlaybackStream::kOutputBytes == 1920, "10 ms stereo PCM");
}

bool AudioPlaybackStream::allocate() {
  if (!storage_) storage_ = static_cast<uint8_t*>(ps_malloc(kBufferBytes + 1));
  if (!storage_) return false;
  if (!stream_) stream_ = xStreamBufferCreateStatic(
      kBufferBytes + 1, 1, storage_, &stream_control_);
  for (auto& buffer : output_) {
    if (!buffer) buffer = static_cast<uint8_t*>(heap_caps_calloc(
        1, kOutputBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    if (!buffer) return false;
  }
  if (!idle_) idle_ = xSemaphoreCreateBinary();
  if (!idle_ || !stream_) return false;
  if (!task_ && xTaskCreatePinnedToCore(taskEntry, "s3-pcm-tx", 6144, this,
                                      4, &task_, 1) != pdPASS) return false;
  return true;
}

bool AudioPlaybackStream::configure() {
  close();
  if (!allocate()) return false;
  i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  channel.dma_desc_num = kDmaDescriptors;
  channel.dma_frame_num = kDmaFrames;
  // 耗尽时输出零，禁止重播上一次 DMA 描述符中的人声。
  channel.auto_clear = true;
  esp_err_t result = i2s_new_channel(&channel, &tx_, nullptr);
  if (result != ESP_OK) { error_.store(result); return false; }
  i2s_std_config_t config{};
  config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
  // S3 无 APLL；160 MHz / (13 + 1/48) = 12.288 MHz，256 × 48 kHz。
  config.clk_cfg.clk_src = I2S_CLK_SRC_PLL_160M;
  config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
      I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  config.gpio_cfg.mclk = static_cast<gpio_num_t>(tkeyboard::pins::i2s_mclk);
  config.gpio_cfg.bclk = static_cast<gpio_num_t>(tkeyboard::pins::i2s_bclk);
  config.gpio_cfg.ws = static_cast<gpio_num_t>(tkeyboard::pins::i2s_ws);
  config.gpio_cfg.dout = static_cast<gpio_num_t>(tkeyboard::pins::i2s_dout);
  config.gpio_cfg.din = I2S_GPIO_UNUSED;  // 仅 TX，不能留下无人消费的 RX DMA。
  result = i2s_channel_init_std_mode(tx_, &config);
  if (result != ESP_OK) {
    error_.store(result);
    i2s_del_channel(tx_);
    tx_ = nullptr;
    return false;
  }
  // 保持 BCLK/MCLK，供 codec 初始化及非流式提示音使用。
  result = i2s_channel_enable(tx_);
  error_.store(result);
  if (result != ESP_OK) { close(); return false; }
  return true;
}

void AudioPlaybackStream::stop() {
  cancelled_.store(true);
  if (worker_started_) {
    // worker 的驱动写入最多等待 20 ms；回收确认前绝不释放 PCM 或 I2S。
    xSemaphoreTake(idle_, portMAX_DELAY);
    worker_started_ = false;
  }
  if (stream_) xStreamBufferReset(stream_);
}

void AudioPlaybackStream::close() {
  stop();
  if (tx_) {
    i2s_channel_disable(tx_);
    i2s_del_channel(tx_);
    tx_ = nullptr;
  }
}

bool AudioPlaybackStream::start() {
  stop();
  if (!tx_ || !stream_ || !task_) return false;
  if (xStreamBufferReset(stream_) != pdPASS) return false;
  written_.store(0);
  underruns_.store(0);
  error_.store(ESP_OK);
  ended_.store(false);
  cancelled_.store(false);
  state_.store(State::Buffering);
  worker_started_ = true;
  xTaskNotifyGive(task_);
  return true;
}

size_t AudioPlaybackStream::buffered() const {
  return stream_ ? xStreamBufferBytesAvailable(stream_) : 0;
}

bool AudioPlaybackStream::enqueue(const uint8_t* pcm, size_t bytes) {
  if (!worker_started_ || cancelled_.load() || ended_.load() ||
      state() == State::Failed || xStreamBufferSpacesAvailable(stream_) < bytes)
    return false;
  // 空间充足才整块准入；不能部分写入后再重试同一 seq。
  return xStreamBufferSend(stream_, pcm, bytes, 0) == bytes;
}

bool AudioPlaybackStream::clockInfo(i2s_chan_info_t& info) const {
  return tx_ && i2s_channel_get_info(tx_, &info) == ESP_OK;
}

size_t AudioPlaybackStream::writeCue(const uint8_t* data, size_t bytes) {
  if (!tx_ || worker_started_) return 0;
  size_t written = 0;
  const esp_err_t result = i2s_channel_write(tx_, data, bytes, &written, 100);
  error_.store(result);
  return written;
}

void AudioPlaybackStream::taskEntry(void* context) {
  auto* self = static_cast<AudioPlaybackStream*>(context);
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    self->run();
    xSemaphoreGive(self->idle_);
  }
}

void AudioPlaybackStream::run() {
  int16_t mono[kInputSamples];
  size_t slot = 0;
  bool buffering = true;
  bool started = false;
  while (!cancelled_.load()) {
    const bool ended = ended_.load();
    const size_t available = buffered();
    if (ended && available == 0) {
      // 写入完成不等于 DMA 播完；等待全部描述符排空，仍支持取消。
      const uint32_t since = millis();
      while (!cancelled_.load() && millis() - since < kDrainMs) vTaskDelay(1);
      if (!cancelled_.load()) state_.store(State::Completed);
      break;
    }
    const size_t threshold = started ? kResumeBytes : kStartBytes;
    if (buffering && available < threshold && !ended) {
      vTaskDelay(1);
      continue;
    }
    if (available == 0) {
      if (!buffering) underruns_.fetch_add(1);
      buffering = true;
      vTaskDelay(1);
      continue;
    }
    buffering = false;
    if (!started) {
      digitalWrite(tkeyboard::pins::pa_enable, HIGH);
      vTaskDelay(pdMS_TO_TICKS(12));
      if (cancelled_.load()) break;
    }
    started = true;
    state_.store(State::Playing);
    const size_t wanted = available < sizeof(mono) ? available : sizeof(mono);
    const size_t bytes = xStreamBufferReceive(stream_, mono, wanted, 0);
    if (!bytes) continue;
    auto* stereo = reinterpret_cast<int16_t*>(output_[slot]);
    for (size_t i = 0; i < bytes / sizeof(int16_t); ++i) {
      for (size_t repeat = 0; repeat < 3; ++repeat) {
        const size_t frame = (i * 3 + repeat) * 2;
        stereo[frame] = mono[i];
        stereo[frame + 1] = mono[i];
      }
    }
    const size_t output_bytes = bytes * 3 * 2;
    size_t offset = 0;
    const uint32_t write_started = millis();
    while (offset < output_bytes && !cancelled_.load()) {
      size_t written = 0;
      const esp_err_t result = i2s_channel_write(
          tx_, output_[slot] + offset, output_bytes - offset, &written, kWriteTimeoutMs);
      offset += written;
      written_.fetch_add(written);
      // 仅续写本次已知进度后的尾部，不重播已交给 DMA 的数据。
      if ((result != ESP_OK && result != ESP_ERR_TIMEOUT) ||
          (offset < output_bytes && millis() - write_started >= 200)) {
        error_.store(result == ESP_OK ? ESP_ERR_TIMEOUT : result);
        state_.store(State::Failed);
        return;
      }
    }
    slot ^= 1;  // DMA 消费前一块时，CPU 在另一块展开下一段 PCM。
  }
}
