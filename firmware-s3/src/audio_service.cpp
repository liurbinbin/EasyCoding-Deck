#include "audio_service.h"

#include <Wire.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include <cmath>
#include <cstring>

#include "pin_config.h"
#include "protocol_transport.h"

namespace {
constexpr uint8_t kEs7210 = 0x40;
constexpr uint8_t kEs8389 = 0x10;
constexpr uint32_t kSampleRate = 16000;
constexpr uint32_t kCodecSampleRate = 48000;
constexpr size_t kInputFrames = 960;  // 48 kHz input, downsampled 3:1 to 16 kHz.
constexpr size_t kOutputFrames = 320;
constexpr char kMasterVolumeEngine[] = "p4-master-volume-v1";
constexpr uint64_t kFnv1a64Offset = 14695981039346656037ULL;
constexpr size_t kMaxPlaybackStreamBytes = 16000 * 2 * 300;
constexpr size_t kPlaybackBufferBytes = AudioPlaybackStream::kBufferBytes;
constexpr size_t kMaxPlaybackChunkBytes = 4096;
constexpr uint8_t kAuxProbeWarmupFrames = 5;
constexpr uint8_t kAuxSignalFramesRequired = 3;
constexpr uint16_t kAuxSignalRmsThreshold = 96;

int16_t upperSample(uint32_t word) {
  return static_cast<int16_t>(word >> 16);
}

int16_t lowerSample(uint32_t word) {
  return static_cast<int16_t>(word & 0xffffU);
}

int16_t mixCaptureFrame(uint32_t left, uint32_t right, bool include_aux) {
  int32_t sum = static_cast<int32_t>(upperSample(left)) +
                static_cast<int32_t>(upperSample(right));
  if (include_aux) sum += static_cast<int32_t>(lowerSample(left));
  return static_cast<int16_t>(sum / (include_aux ? 3 : 2));
}

uint16_t auxRms(const uint32_t* samples) {
  uint64_t energy = 0;
  for (size_t i = 0; i < kOutputFrames; ++i) {
    const int32_t sample = lowerSample(samples[i * 3 * 2]);
    energy += static_cast<uint64_t>(sample * sample);
  }
  return static_cast<uint16_t>(sqrt(static_cast<double>(energy) / kOutputFrames));
}

String base64(const uint8_t* data, size_t length) {
  static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String out;
  out.reserve(((length + 2) / 3) * 4);
  for (size_t i = 0; i < length; i += 3) {
    const uint32_t value = (static_cast<uint32_t>(data[i]) << 16) |
        (i + 1 < length ? static_cast<uint32_t>(data[i + 1]) << 8 : 0) |
        (i + 2 < length ? data[i + 2] : 0);
    out += table[(value >> 18) & 63];
    out += table[(value >> 12) & 63];
    out += i + 1 < length ? table[(value >> 6) & 63] : '=';
    out += i + 2 < length ? table[value & 63] : '=';
  }
  return out;
}

uint64_t fnv1a(const uint8_t* data, size_t length, uint64_t seed = kFnv1a64Offset) {
  uint64_t hash = seed;
  for (size_t i = 0; i < length; ++i) { hash ^= data[i]; hash *= 1099511628211ULL; }
  return hash;
}

String checksum(uint64_t value) {
  char text[17];
  snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
  return text;
}

struct Base64DecodeResult {
  int status = MBEDTLS_ERR_BASE64_INVALID_CHARACTER;
  size_t required = 0;
  size_t decoded = 0;
};

Base64DecodeResult decodeBase64(const char* input, uint8_t* output,
                                size_t capacity) {
  Base64DecodeResult result;
  if (!input || !*input || !output) return result;
  const auto* encoded = reinterpret_cast<const unsigned char*>(input);
  const size_t encoded_bytes = strlen(input);
  result.status = mbedtls_base64_decode(
      nullptr, 0, &result.required, encoded, encoded_bytes);
  if (result.status != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL ||
      result.required > capacity)
    return result;
  result.status = mbedtls_base64_decode(
      output, capacity, &result.decoded, encoded, encoded_bytes);
  return result;
}
}

bool AudioService::writeRegister(uint8_t address, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool AudioService::updateRegister(uint8_t address, uint8_t reg, uint8_t mask, uint8_t value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(address, static_cast<uint8_t>(1)) != 1) return false;
  const uint8_t current = Wire.read();
  return writeRegister(address, reg, static_cast<uint8_t>((current & ~mask) | (value & mask)));
}

bool AudioService::configureCaptureCodec(bool include_aux) {
  if (!writeRegister(kEs7210, 0x00, 0xFF) || !writeRegister(kEs7210, 0x00, 0x41)) return false;
  const uint8_t init[][2] = {
      {0x01, static_cast<uint8_t>(include_aux ? 0x20 : 0x14)},
      {0x02, 0xC1}, {0x06, 0x00}, {0x07, 0x20}, {0x08, 0x10},
      {0x09, 0x30}, {0x0A, 0x30}, {0x10, 0x00}, {0x11, 0x60}, {0x12, 0x00},
      {0x16, 0x00}, {0x1A, 0x00}, {0x1B, 0xBF}, {0x1C, 0xBF},
      {0x1D, 0xBF}, {0x1E, 0xBF}, {0x40, 0x43}, {0x41, 0x70}, {0x42, 0x70},
      {0x43, 0x1A}, {0x44, 0x1A},
  };
  for (const auto& entry : init) if (!writeRegister(kEs7210, entry[0], entry[1])) return false;
  const uint8_t mode[][2] = {
      {0x12, static_cast<uint8_t>(include_aux ? 0x02 : 0x00)},
      {0x45, static_cast<uint8_t>(include_aux ? 0x1A : 0x00)},
      {0x46, static_cast<uint8_t>(include_aux ? 0x10 : 0x00)},
      {0x47, 0x08}, {0x48, 0x08}, {0x49, 0x08}, {0x4A, 0x08},
      {0x4B, 0x00}, {0x4C, static_cast<uint8_t>(include_aux ? 0x00 : 0xFF)},
  };
  for (const auto& entry : mode) if (!writeRegister(kEs7210, entry[0], entry[1])) return false;
  return writeRegister(kEs7210, 0x00, 0x71) && writeRegister(kEs7210, 0x00, 0x41);
}

bool AudioService::stopCaptureCodec() {
  // Playback mode from the hardware acceptance firmware: stop every ES7210
  // ADC path so an unread RX DMA queue cannot stall the shared I2S controller.
  return writeRegister(kEs7210, 0x4B, 0xFF) &&
         writeRegister(kEs7210, 0x4C, 0xFF) &&
         writeRegister(kEs7210, 0x40, 0xC0) &&
         writeRegister(kEs7210, 0x01, 0x34) &&
         writeRegister(kEs7210, 0x06, 0x07);
}

bool AudioService::configurePlaybackCodec() {
  Wire.beginTransmission(kEs8389);
  if (Wire.endTransmission() != 0) return false;
  const uint8_t init[][2] = {
      {0xF3, 0x00}, {0x00, 0x7E}, {0xF3, 0x38}, {0x24, 0x64}, {0x25, 0x04},
      {0x45, 0x03}, {0x60, 0x2A}, {0x61, 0xC9}, {0x62, 0x4F}, {0x63, 0x06},
      {0x6B, 0x00}, {0x6D, 0x16}, {0x6E, 0xAA}, {0x6F, 0x66}, {0x70, 0x99},
      {0x23, 0x00}, {0x72, 0x10}, {0x73, 0x10}, {0x10, 0xC4}, {0x01, 0x08},
      {0xF1, 0x00}, {0x12, 0x01}, {0x13, 0x01}, {0x14, 0x01}, {0x15, 0x01},
      {0x16, 0x35}, {0x17, 0x09}, {0x18, 0x91}, {0x19, 0x28}, {0x1A, 0x01},
      {0x1B, 0x01}, {0x1C, 0x11}, {0xF0, 0x12}, {0x02, 0x00}, {0x04, 0x00},
      {0x05, 0x10}, {0x06, 0x00}, {0x07, 0xC0}, {0x08, 0x00}, {0x09, 0xC0},
      {0x0A, 0x80}, {0x0B, 0x04}, {0x0C, 0x01}, {0x0D, 0x00}, {0x0F, 0x00},
      {0x21, 0x1F}, {0x22, 0x7F}, {0x2F, 0xC0}, {0x30, 0xF4}, {0x31, 0x00},
      {0x41, 0x7F}, {0x42, 0x7F}, {0x43, 0x10}, {0x44, 0x00}, {0x49, 0x0F},
      {0x4C, 0xC0}, {0x00, 0x00}, {0x03, 0xC1}, {0x00, 0x01}, {0x4D, 0x02},
  };
  for (const auto& entry : init) if (!writeRegister(kEs8389, entry[0], entry[1])) return false;
  const uint8_t clock48k[][2] = {
      {0x02, 0x00}, {0x04, 0x01}, {0x05, 0x41}, {0x06, 0x04}, {0x07, 0xD0},
      {0x08, 0x10}, {0x09, 0xD1}, {0x0A, 0x80}, {0x0F, 0x50}, {0x11, 0x00},
      {0x21, 0x1F}, {0x22, 0x7F}, {0x26, 0xBF}, {0x30, 0xF4}, {0x41, 0x7F},
      {0x42, 0x7F}, {0x43, 0x10}, {0xF0, 0x12}, {0xF1, 0x00}, {0x16, 0x35},
      {0x18, 0x91}, {0x19, 0x28},
  };
  for (const auto& entry : clock48k) {
    bool ok = true;
    if (entry[0] == 0x0F || entry[0] == 0x30)
      ok = updateRegister(kEs8389, entry[0], 0xC0, entry[1] & 0xC0);
    else if (entry[0] == 0x43)
      ok = updateRegister(kEs8389, entry[0], 0x81, entry[1] & 0x81);
    else
      ok = writeRegister(kEs8389, entry[0], entry[1]);
    if (!ok) return false;
  }
  if (!updateRegister(kEs8389, 0x20, 0x7F, 0x60) ||
      !updateRegister(kEs8389, 0x40, 0x7F, 0x60) ||
      !writeRegister(kEs8389, 0x27, 0xBF) || !writeRegister(kEs8389, 0x28, 0xBF) ||
      !writeRegister(kEs8389, 0x46, 0xB0) || !writeRegister(kEs8389, 0x47, 0xB0) ||
      !writeRegister(kEs8389, 0x48, 0xBE) || !updateRegister(kEs8389, 0x69, 0x20, 0x20) ||
      !writeRegister(kEs8389, 0x61, 0xD9) || !writeRegister(kEs8389, 0x64, 0x8F) ||
      !writeRegister(kEs8389, 0x10, 0xE4) || !writeRegister(kEs8389, 0x03, 0xC3)) return false;
  delay(72);
  return writeRegister(kEs8389, 0x4D, 0x00) && applyVolume(volume_);
}

bool AudioService::applyVolume(uint8_t volume) {
  if (volume > 100) return false;
  // ES8389 DAC volume is 0.5 dB per step: 0x20 is about -79.5 dB and
  // 0xBF is 0 dB. Reserve zero for effective mute and spread 1..100 over
  // the useful range without exceeding the codec's unity-gain setting.
  const uint8_t reg = volume == 0 ? 0 : static_cast<uint8_t>(0x20 + (volume - 1) * 0x9F / 99);
  const uint8_t mute = volume == 0 ? 0x80 : 0x00;
  return updateRegister(kEs8389, 0x20, 0x80, mute) &&
         updateRegister(kEs8389, 0x40, 0x80, mute) &&
         writeRegister(kEs8389, 0x46, reg) && writeRegister(kEs8389, 0x47, reg);
}

void AudioService::notifyEvent(const char* event) {
  if (!event || !*event || volume_ == 0) return;
  // Keep only the newest lifecycle notification while capture or streamed
  // playback owns the codec. It is played as soon as the shared bus is free.
  pending_event_cue_ = event;
}

bool AudioService::playEventCue(const char* event) {
  if (!event || !*event || active_ || playback_ || volume_ == 0 || !playback_available_) return false;
  if (!startPlaybackBus() || !configurePlaybackCodec()) return false;

  constexpr size_t kFrames = 192;
  int16_t samples[kFrames * 2];
  double phase = 0.0;
  auto write_tone = [&](uint16_t frequency, uint16_t duration_ms, int16_t amplitude) {
    size_t remaining = static_cast<size_t>(kCodecSampleRate) * duration_ms / 1000;
    while (remaining > 0) {
      const size_t frames = remaining > kFrames ? kFrames : remaining;
      for (size_t i = 0; i < frames; ++i) {
        const int16_t value = frequency == 0 ? 0 :
            static_cast<int16_t>(sinf(static_cast<float>(phase)) * amplitude);
        samples[i * 2] = value;
        samples[i * 2 + 1] = value;
        phase += 2.0 * PI * frequency / kCodecSampleRate;
        if (phase >= 2.0 * PI) phase -= 2.0 * PI;
      }
      const size_t bytes = frames * 2 * sizeof(int16_t);
      if (playback_stream_.writeCue(reinterpret_cast<uint8_t*>(samples), bytes) != bytes) return false;
      remaining -= frames;
    }
    return true;
  };

  digitalWrite(tkeyboard::pins::pa_enable, HIGH);
  delay(12);
  bool ok = true;
  if (!strcmp(event, "done")) {
    ok = write_tone(660, 85, 9000) && write_tone(0, 35, 0) && write_tone(990, 145, 10000);
  } else if (!strcmp(event, "error")) {
    ok = write_tone(330, 150, 9000) && write_tone(0, 45, 0) && write_tone(220, 220, 10000);
  } else if (!strcmp(event, "permission_approved")) {
    ok = write_tone(740, 70, 8500) && write_tone(0, 25, 0) && write_tone(1110, 125, 10000);
  } else if (!strcmp(event, "permission_denied")) {
    ok = write_tone(280, 220, 10000);
  } else if (!strcmp(event, "permission_expired")) {
    ok = write_tone(440, 90, 7500) && write_tone(0, 45, 0) && write_tone(440, 90, 7500);
  } else {
    // waiting_user and permission_request use a distinct double alert.
    ok = write_tone(880, 80, 9000) && write_tone(0, 55, 0) && write_tone(880, 135, 9000);
  }
  delay(12);
  digitalWrite(tkeyboard::pins::pa_enable, LOW);
  return ok;
}

bool AudioService::startCaptureBus() {
  playback_stream_.close();
  i2s_.end();
  pinMode(tkeyboard::pins::i2s_dout, OUTPUT);
  digitalWrite(tkeyboard::pins::i2s_dout, LOW);
  i2s_.setPins(tkeyboard::pins::i2s_bclk, tkeyboard::pins::i2s_ws,
               -1, tkeyboard::pins::i2s_din, tkeyboard::pins::i2s_mclk);
  const bool ok = i2s_.begin(I2S_MODE_STD, kCodecSampleRate, I2S_DATA_BIT_WIDTH_32BIT,
                             I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH);
  i2s_.setTimeout(100);
  playback_bus_ = false;
  return ok;
}

bool AudioService::startPlaybackBus() {
  i2s_.end();
  playback_bus_ = playback_stream_.configure();
  return playback_bus_;
}

void AudioService::begin() {
  pinMode(tkeyboard::pins::pa_enable, OUTPUT);
  digitalWrite(tkeyboard::pins::pa_enable, LOW);
  pinMode(tkeyboard::pins::aux_detect, INPUT_PULLUP);
  capture_raw_ = static_cast<uint32_t*>(ps_malloc(kInputFrames * 2 * sizeof(uint32_t)));
  capture_pcm_ = static_cast<int16_t*>(ps_malloc(kOutputFrames * sizeof(int16_t)));
  Preferences preferences;
  if (preferences.begin("audio", false)) {
    if (preferences.isKey("volume")) {
      const uint8_t saved = preferences.getUChar("volume", 75);
      volume_ = saved > 100 ? 100 : saved;
      volume_persisted_ = true;
    } else {
      volume_persisted_ = preferences.putUChar("volume", volume_) == 1;
    }
    preferences.end();
  }
  const bool playback_bus = startPlaybackBus();
  playback_available_ = playback_bus && configurePlaybackCodec();
  initialized_ = capture_raw_ && capture_pcm_ && playback_available_ && configureCaptureCodec(false);
  playback_available_ = initialized_ && stopCaptureCodec();
  sendStatus(initialized_ ? "initialized" : "capture-unavailable");
}

bool AudioService::send(const char* topic, JsonDocument& doc) {
  doc["topic"] = topic;
  return protocolTransport().sendDocument(doc);
}

void AudioService::sendStatus(const char* reason) {
  JsonDocument doc;
  auto payload = doc["payload"].to<JsonObject>();
  payload["active"] = active_;
  payload["sessionId"] = session_id_;
  payload["sampleRate"] = kSampleRate;
  payload["channels"] = 1;
  payload["codec"] = initialized_ ? "ES7210" : "unavailable";
  payload["available"] = initialized_;
  payload["captureAvailable"] = initialized_;
  payload["playbackAvailable"] = playback_available_;
  payload["playbackActive"] = playback_;
  payload["streamActive"] = active_;
  payload["conversation"] = conversation_active_;
  payload["conversationSessionId"] = conversation_session_;
  // GPIO6 is only an insertion hint on this board. Some valid TRRS plugs do
  // not pull it low, so retain signal-based MIC3 detection after capture.
  const bool aux_inserted = digitalRead(tkeyboard::pins::aux_detect) == LOW ||
                            capture_uses_mic3_;
  const bool use_aux = capture_uses_mic3_ || aux_inserted;
  payload["auxMicInserted"] = aux_inserted;
  payload["captureInputChannels"] = use_aux ? 3 : 2;
  payload["captureInputMode"] = use_aux ? "mic1+mic2+mic3" : "mic1+mic2";
  payload["audioAecPreparing"] = false;
  payload["audioAec"] = false;
  payload["playbackBufferedBytes"] = playback_stream_.buffered();
  payload["playbackUnderruns"] = playback_stream_.underruns();
  payload["playbackDmaDescriptors"] = AudioPlaybackStream::kDmaDescriptors;
  payload["playbackDmaFrames"] = AudioPlaybackStream::kDmaFrames;
  payload["playbackDmaMs"] = AudioPlaybackStream::kDmaDurationMs;
  i2s_chan_info_t clock{};
  if (playback_stream_.clockInfo(clock)) {
    // 驱动分频结果，不是逻辑分析仪测得的物理频率。
    payload["playbackClockSource"] = "pll-160m";
    payload["playbackSourceHz"] = clock.sclk_hz;
    payload["playbackMclkHz"] = clock.mclk_hz;
    payload["playbackBclkHz"] = clock.bclk_hz;
    payload["playbackDmaBytes"] = clock.total_dma_buf_size;
    payload["playbackSampleRateHz"] = clock.bclk_hz / (16 * 2);
  }
  payload["reason"] = reason;
  send("audio/status", doc);
}

void AudioService::sendError(const char* code, const char* message) {
  JsonDocument doc;
  auto payload = doc["payload"].to<JsonObject>();
  payload["boardDeviceId"] = String("s3-tkeyboard-") + String((uint32_t)ESP.getEfuseMac(), HEX);
  payload["sessionId"] = session_id_;
  payload["code"] = code;
  payload["error"] = message;
  send("audio/error", doc);
  sendStatus(code);
}

void AudioService::rejectCapture(const char* code, const char* message) {
  sendError(code, message);
}

void AudioService::rejectPlayback(JsonObjectConst request, const char* message) {
  sendPlaybackState(request, false, message);
}

bool AudioService::startCapture(const char* session_id) {
  if (!initialized_) {
    sendError("capture-unavailable", "ES7210 microphone capture is unavailable");
    return false;
  }
  if (active_) {
    sendStatus("already-capturing");
    return true;
  }
  if (playback_) {
    sendError("playback-active", "microphone capture cannot start during playback");
    return false;
  }
  capture_uses_mic3_ = digitalRead(tkeyboard::pins::aux_detect) == LOW;
  aux_probe_frames_ = 0;
  aux_signal_frames_ = 0;
  if (!startCaptureBus()) {
    sendError("capture-bus-unavailable", "I2S microphone bus is unavailable");
    return false;
  }
  // Always expose MIC3 in the packed TDM stream. Mixing remains dual-mic
  // until GPIO6 or consecutive MIC3 audio proves that AUX is actually used.
  if (!configureCaptureCodec(true)) {
    startPlaybackBus();
    sendError("capture-codec-unavailable", "ES7210 microphone codec is unavailable");
    return false;
  }
  session_id_ = session_id && *session_id ? session_id : String("s3-audio-") + millis();
  sequence_ = 0;
  bytes_sent_ = 0;
  capture_started_at_ = millis();
  stream_checksum_ = kFnv1a64Offset;
  active_ = true;
  JsonDocument doc;
  auto payload = doc["payload"].to<JsonObject>();
  payload["boardDeviceId"] = String("s3-tkeyboard-") + String((uint32_t)ESP.getEfuseMac(), HEX);
  payload["sessionId"] = session_id_;
  if (conversation_active_) payload["conversationSessionId"] = conversation_session_;
  payload["format"] = "pcm_s16le";
  payload["sampleRate"] = kSampleRate;
  payload["channels"] = 1;
  payload["bitsPerSample"] = 16;
  payload["frameMs"] = 20;
  payload["captureInputChannels"] = capture_uses_mic3_ ? 3 : 2;
  payload["captureInputMode"] = capture_uses_mic3_ ? "mic1+mic2+mic3" : "mic1+mic2";
  payload["transport"] = protocolTransport().owner().kind == TransportKind::Wifi
      ? "wifi-websocket-json-pcm-v1" : "usb-jsonl-pcm-v1";
  send("audio/begin", doc);
  sendStatus("capture-started");
  return true;
}

void AudioService::stopCapture(const char* reason) {
  if (!active_) return;
  active_ = false;
  JsonDocument doc;
  auto payload = doc["payload"].to<JsonObject>();
  payload["sessionId"] = session_id_;
  payload["reason"] = reason;
  payload["chunks"] = sequence_;
  payload["bytes"] = bytes_sent_;
  payload["durationMs"] = millis() - capture_started_at_;
  payload["checksum"] = checksum(stream_checksum_);
  send("audio/end", doc);
  const bool restored = stopCaptureCodec() && startPlaybackBus();
  sendStatus(restored ? reason : "playback-bus-unavailable");
}

bool AudioService::suspendConversationCapture() {
  if (!active_) return true;
  active_ = false;
  return stopCaptureCodec() && startPlaybackBus();
}

void AudioService::resumeConversationCapture() {
  if (!resume_capture_after_playback_) return;
  resume_capture_after_playback_ = false;
  if (conversation_active_ && conversation_half_duplex_)
    startCapture(conversation_session_.c_str());
}

bool AudioService::handleConversation(JsonObjectConst payload) {
  const bool enabled = payload["enabled"] | false;
  if (!enabled) {
    conversation_active_ = false;
    resume_capture_after_playback_ = false;
    conversation_session_ = "";
    // disable 本身必须能停止扬声器，不能依赖另一条 flush 已经到达。
    if (playback_) finishPlayback(true, nullptr, false);
    if (active_) stopCapture("conversation-ended");
    else sendStatus("conversation-ended");
    return true;
  }
  const String session = payload["sessionId"] | "";
  const bool half_duplex = payload["halfDuplex"] | true;
  if (session.isEmpty() || !half_duplex) {
    sendError("conversation-unsupported", "S3 realtime conversation requires half duplex");
    return false;
  }
  conversation_active_ = true;
  conversation_half_duplex_ = true;
  conversation_session_ = session;
  if (!startCapture(conversation_session_.c_str())) {
    conversation_active_ = false;
    conversation_session_ = "";
    return false;
  }
  sendStatus("conversation-started");
  return true;
}

void AudioService::beginPlayback(JsonObjectConst payload) {
  if (!playback_available_) {
    sendPlaybackState(payload, false, "ES8389 playback is unavailable");
    return;
  }
  if (playback_) {
    sendPlaybackState(payload, false, "another playback session is active");
    return;
  }
  if (active_ && conversation_active_ && conversation_half_duplex_) {
    resume_capture_after_playback_ = true;
    if (!suspendConversationCapture()) {
      resume_capture_after_playback_ = false;
      sendPlaybackState(payload, false, "microphone suspension failed");
      return;
    }
  } else if (active_) {
    sendPlaybackState(payload, false, "microphone capture is active");
    return;
  }
  if (volume_ == 0) {
    resumeConversationCapture();
    sendPlaybackState(payload, false, "device volume is muted");
    return;
  }
  playback_session_ = payload["sessionId"] | "";
  if (playback_session_.isEmpty()) {
    resumeConversationCapture();
    sendPlaybackState(payload, false, "playback session is missing");
    return;
  }
  const size_t expected = payload["bytes"] | 0;
  if (expected > kMaxPlaybackStreamBytes || (expected & 1) != 0) {
    playback_session_ = "";
    resumeConversationCapture();
    sendPlaybackState(payload, false, "playback byte count is invalid");
    return;
  }
  if (!startPlaybackBus() || !configurePlaybackCodec()) {
    playback_session_ = "";
    resumeConversationCapture();
    sendPlaybackState(payload, false, "ES8389 playback initialization failed");
    return;
  }
  releasePlaybackBuffer();
  playback_capacity_bytes_ = kPlaybackBufferBytes;
  playback_decode_ = static_cast<uint8_t*>(ps_malloc(kMaxPlaybackChunkBytes));
  if (!playback_decode_ || !playback_stream_.start()) {
    releasePlaybackBuffer();
    playback_session_ = "";
    resumeConversationCapture();
    sendPlaybackState(payload, false, "playback PSRAM allocation failed");
    return;
  }
  playback_expected_bytes_ = expected;
  playback_buffered_bytes_ = 0;
  playback_received_bytes_ = 0;
  playback_sequence_.reset();
  playback_output_active_ = false;
  playback_end_received_ = false;
  playback_written_bytes_ = 0;
  playback_end_request_id_ = "";
  playback_owner_ = protocolTransport().owner();
  playback_ = true;
  sendPlaybackState(payload, true);
  sendStatus("playback-buffering");
}

void AudioService::failPlayback(JsonObjectConst request, const char* code,
                                const char* message, size_t declared_bytes,
                                size_t encoded_bytes, size_t decoded_bytes,
                                int decode_status) {
  JsonDocument error;
  auto response = error["payload"].to<JsonObject>();
  response["boardDeviceId"] = String("s3-tkeyboard-") +
                              String((uint32_t)ESP.getEfuseMac(), HEX);
  response["requestId"] = request["requestId"] | "";
  response["sessionId"] = playback_session_;
  response["code"] = code;
  response["error"] = message;
  if (declared_bytes > 0) response["declaredBytes"] = declared_bytes;
  if (encoded_bytes > 0) response["encodedBytes"] = encoded_bytes;
  if (decoded_bytes > 0) response["decodedBytes"] = decoded_bytes;
  if (decode_status != 0) response["decodeStatus"] = decode_status;
  playback_buffered_bytes_ = playback_stream_.buffered();
  playback_written_bytes_ = playback_stream_.written();
  response["bufferedBytes"] = playback_buffered_bytes_;
  response["expectedEncodedBytes"] = ((declared_bytes + 2) / 3) * 4;
  response["underruns"] = playback_stream_.underruns();
  response["bufferCapacityBytes"] = playback_capacity_bytes_;
  response["receivedBytes"] = playback_received_bytes_;
  response["writtenBytes"] = playback_written_bytes_;
  response["transport"] = playback_owner_.kind == TransportKind::Wifi
      ? "wifi" : "usb";
  send("audio/error", error);
  playback_end_request_id_ = request["requestId"] | "";
  finishPlayback(false, message, false);
}

void AudioService::writePlayback(JsonObjectConst payload) {
  if (!playback_ || !playback_decode_ ||
      String(payload["sessionId"] | "") != playback_session_ ||
      !playback_owner_.sameConnection(protocolTransport().owner())) {
    sendPlaybackState(payload, false, "playback session is not current",
                      playback_received_bytes_, playback_written_bytes_);
    return;
  }
  if (playback_end_received_) {
    sendPlaybackState(payload, false, "playback no longer accepts chunks",
                      playback_received_bytes_, playback_written_bytes_);
    return;
  }
  if (!payload["seq"].is<uint32_t>() ||
      !playback_sequence_.accept(payload["seq"].as<uint32_t>())) {
    failPlayback(payload, "playback-sequence-invalid",
                 "PCM playback chunk sequence is invalid");
    return;
  }
  const size_t declared = payload["bytes"] | 0;
  if (!declared || (declared & 1) != 0 ||
      declared > kMaxPlaybackChunkBytes ||
      declared > kMaxPlaybackStreamBytes - playback_received_bytes_ ||
      (playback_expected_bytes_ &&
       declared > playback_expected_bytes_ - playback_received_bytes_)) {
    failPlayback(payload, "playback-chunk-invalid",
                 "PCM playback chunk exceeds the declared limit", declared);
    return;
  }
  if (!payload["data"].is<const char*>()) {
    failPlayback(payload, "playback-base64-missing",
                 "PCM playback chunk data is missing", declared);
    return;
  }
  const char* encoded = payload["data"].as<const char*>();
  const size_t encoded_bytes = strlen(encoded);
  const size_t expected_encoded_bytes = ((declared + 2) / 3) * 4;
  if (encoded_bytes != expected_encoded_bytes) {
    failPlayback(payload, "playback-base64-length-mismatch",
                 "PCM playback chunk encoded length does not match bytes",
                 declared, encoded_bytes);
    return;
  }
  const Base64DecodeResult decoded = decodeBase64(
      encoded, playback_decode_, kMaxPlaybackChunkBytes);
  if (decoded.status != 0) {
    failPlayback(payload, "playback-base64-invalid",
                 "PCM playback chunk base64 is invalid", declared,
                 encoded_bytes, decoded.required, decoded.status);
    return;
  }
  if (decoded.required != declared || decoded.decoded != declared) {
    failPlayback(payload, "playback-decoded-length-mismatch",
                 "PCM playback chunk decoded length does not match bytes",
                 declared, encoded_bytes,
                 decoded.decoded ? decoded.decoded : decoded.required);
    return;
  }

  if (!playback_stream_.enqueue(playback_decode_, declared)) {
    failPlayback(payload, "playback-buffer-full",
                 "PCM playback streaming buffer is full or DMA worker failed", declared);
    return;
  }
  playback_buffered_bytes_ = playback_stream_.buffered();
  playback_received_bytes_ += declared;
}

void AudioService::releasePlaybackBuffer() {
  playback_stream_.stop();
  if (playback_decode_) free(playback_decode_);
  playback_decode_ = nullptr;
  playback_expected_bytes_ = 0;
  playback_capacity_bytes_ = 0;
  playback_buffered_bytes_ = 0;
  playback_received_bytes_ = 0;
  playback_sequence_.reset();
  playback_output_active_ = false;
  playback_end_received_ = false;
  playback_written_bytes_ = 0;
}

void AudioService::endPlayback(JsonObjectConst payload, bool play) {
  const String requested_session = payload["sessionId"] | "";
  if (!playback_) {
    sendPlaybackState(payload, !play,
                      play ? "no playback session is active" : nullptr);
    return;
  }
  const bool conversation_flush = !play && conversation_active_ &&
                                  requested_session == conversation_session_;
  if (requested_session != playback_session_ && !conversation_flush) {
    sendPlaybackState(payload, false, "playback session is not current",
                      playback_received_bytes_, playback_written_bytes_);
    return;
  }
  playback_end_request_id_ = payload["requestId"] | "";
  if (!play) {
    finishPlayback(true, nullptr, false);
    return;
  }
  const size_t declared_total = payload["bytes"] | 0;
  const bool complete = playback_received_bytes_ > 0 &&
                        declared_total == playback_received_bytes_ &&
                        declared_total <= kMaxPlaybackStreamBytes &&
                        (declared_total & 1) == 0 &&
                        (!playback_expected_bytes_ ||
                         playback_received_bytes_ == playback_expected_bytes_);
  if (!complete) {
    finishPlayback(false, "playback data is incomplete", true);
    return;
  }
  if (!playback_owner_.sameConnection(protocolTransport().owner())) {
    finishPlayback(false, "playback transport owner changed", true);
    return;
  }
  playback_end_received_ = true;
  playback_stream_.end();
  sendStatus("playback-draining");
}

void AudioService::pollPlayback() {
  if (!playback_) return;
  if (!playback_owner_.sameConnection(protocolTransport().owner())) {
    finishPlayback(false, "playback transport owner changed", true);
    return;
  }
  playback_buffered_bytes_ = playback_stream_.buffered();
  playback_written_bytes_ = playback_stream_.written();
  const auto state = playback_stream_.state();
  if (!playback_output_active_ && state == AudioPlaybackStream::State::Playing) {
    digitalWrite(tkeyboard::pins::pa_enable, HIGH);
    playback_output_active_ = true;
    sendStatus("playback-playing");
  }
  if (state == AudioPlaybackStream::State::Completed) {
    finishPlayback(true, nullptr, true);
  } else if (state == AudioPlaybackStream::State::Failed) {
    finishPlayback(false, "I2S DMA playback write failed", true);
  }
}

void AudioService::finishPlayback(bool ok, const char* error, bool played) {
  if (!playback_) return;
  const String session = playback_session_;
  playback_stream_.stop();
  playback_written_bytes_ = playback_stream_.written();
  const size_t received_bytes = playback_received_bytes_;
  const size_t written_bytes = playback_written_bytes_;
  const String request_id = playback_end_request_id_;
  playback_ = false;
  playback_output_active_ = false;
  digitalWrite(tkeyboard::pins::pa_enable, LOW);
  releasePlaybackBuffer();
  playback_session_ = "";
  playback_end_request_id_ = "";
  playback_owner_ = {};

  JsonDocument request;
  request["requestId"] = request_id;
  request["sessionId"] = session;
  sendPlaybackState(request.as<JsonObjectConst>(), ok, error, received_bytes,
                    written_bytes);
  if (played) {
    JsonDocument diagnostic;
    auto status = diagnostic["payload"].to<JsonObject>();
    status["boardDeviceId"] = String("s3-tkeyboard-") +
                              String((uint32_t)ESP.getEfuseMac(), HEX);
    status["sessionId"] = session;
    status["conversationSessionId"] = conversation_session_;
    status["event"] = ok ? "playback_completed" : "playback_failed";
    status["bytes"] = received_bytes;
    status["ok"] = ok;
    send("audio/diagnostic", diagnostic);
  }
  resumeConversationCapture();
  sendStatus(ok ? (played ? "playback-ended" : "playback-cancelled")
                : "playback-failed");
}

void AudioService::abortForTransportChange() {
  const bool capture_was_active = active_;
  active_ = false;
  conversation_active_ = false;
  conversation_session_ = "";
  resume_capture_after_playback_ = false;
  if (capture_was_active) {
    stopCaptureCodec();
    startPlaybackBus();
  }
  if (playback_) {
    playback_stream_.stop();
    playback_ = false;
    playback_output_active_ = false;
    digitalWrite(tkeyboard::pins::pa_enable, LOW);
    releasePlaybackBuffer();
    playback_session_ = "";
    playback_end_request_id_ = "";
    playback_owner_ = {};
  }
}

void AudioService::sendPlaybackState(JsonObjectConst request, bool ok, const char* error,
                                     size_t received_bytes, size_t written_bytes) {
  JsonDocument doc;
  auto payload = doc["payload"].to<JsonObject>();
  payload["requestId"] = request["requestId"] | "";
  payload["boardDeviceId"] = String("s3-tkeyboard-") + String((uint32_t)ESP.getEfuseMac(), HEX);
  payload["sessionId"] = request["sessionId"] | "";
  payload["ok"] = ok;
  payload["receivedBytes"] = received_bytes;
  payload["writtenBytes"] = written_bytes;
  payload["volume"] = volume_;
  if (error) payload["error"] = error;
  send("audio/play-state", doc);
}

void AudioService::handleVolume(JsonObjectConst payload) {
  const char* operation = payload["operation"] | "";
  bool ok = playback_available_;
  bool persisted = volume_persisted_;
  const bool setting = !strcmp(operation, "set");
  if (strcmp(operation, "query") && !setting) ok = false;
  if (setting) {
    const int requested = payload["volume"] | -1;
    ok = ok && requested >= 0 && requested <= 100 && applyVolume(static_cast<uint8_t>(requested));
    if (ok) {
      volume_ = static_cast<uint8_t>(requested);
      Preferences preferences;
      if (preferences.begin("audio", false)) {
        persisted = preferences.putUChar("volume", volume_) == 1;
        preferences.end();
      } else {
        persisted = false;
      }
      volume_persisted_ = persisted;
    }
  }
  JsonDocument doc;
  auto response = doc["payload"].to<JsonObject>();
  response["engine"] = kMasterVolumeEngine;
  response["boardDeviceId"] = String("s3-tkeyboard-") + String((uint32_t)ESP.getEfuseMac(), HEX);
  response["requestId"] = payload["requestId"] | "";
  response["ok"] = ok;
  response["volume"] = volume_;
  response["persisted"] = persisted;
  response["savePending"] = false;
  send("audio/volume-state", doc);
}

void AudioService::handle(const char* topic, JsonObjectConst payload) {
  if (!strcmp(topic, "audio/volume")) handleVolume(payload);
  else if (!strcmp(topic, "audio/play_begin")) beginPlayback(payload);
  else if (!strcmp(topic, "audio/play_chunk")) writePlayback(payload);
  else if (!strcmp(topic, "audio/play_end")) endPlayback(payload, true);
  else if (!strcmp(topic, "audio/play_flush")) endPlayback(payload, false);
  else if (!strcmp(topic, "audio/conversation")) handleConversation(payload);
  else if (!strcmp(topic, "audio/query")) sendStatus("query");
  else if (strcmp(topic, "audio/control") != 0) return;
  const char* action = payload["action"] | "";
  if (!strcmp(action, "start")) startCapture(payload["sessionId"] | "");
  else if (!strcmp(action, "stop")) stopCapture(payload["reason"] | "stopped");
}

void AudioService::poll() {
  if (playback_) {
    pollPlayback();
    return;
  }
  if (!active_ && !playback_ && !pending_event_cue_.isEmpty()) {
    const String event = pending_event_cue_;
    pending_event_cue_ = "";
    playEventCue(event.c_str());
    return;
  }
  if (!active_ || !capture_raw_ || !capture_pcm_) return;
  const size_t raw_bytes = kInputFrames * 2 * sizeof(uint32_t);
  const size_t pcm_bytes = kOutputFrames * sizeof(int16_t);
  const size_t received = i2s_.readBytes(reinterpret_cast<char*>(capture_raw_), raw_bytes);
  if (received != raw_bytes) return;
  if (!capture_uses_mic3_) {
    if (digitalRead(tkeyboard::pins::aux_detect) == LOW) {
      capture_uses_mic3_ = true;
    } else if (aux_probe_frames_ >= kAuxProbeWarmupFrames) {
      if (auxRms(capture_raw_) >= kAuxSignalRmsThreshold) {
        if (++aux_signal_frames_ >= kAuxSignalFramesRequired) {
          capture_uses_mic3_ = true;
          sendStatus("mic3-detected");
        }
      } else {
        aux_signal_frames_ = 0;
      }
      if (aux_probe_frames_ != UINT8_MAX) ++aux_probe_frames_;
    } else {
      ++aux_probe_frames_;
    }
  }
  for (size_t i = 0; i < kOutputFrames; ++i) {
    const size_t frame = i * 3 * 2;
    capture_pcm_[i] = mixCaptureFrame(capture_raw_[frame], capture_raw_[frame + 1],
                                      capture_uses_mic3_);
  }
  JsonDocument doc;
  auto payload = doc["payload"].to<JsonObject>();
  const uint8_t* pcm = reinterpret_cast<const uint8_t*>(capture_pcm_);
  const uint64_t frame_checksum = fnv1a(pcm, pcm_bytes);
  const uint64_t next_stream_checksum = fnv1a(pcm, pcm_bytes, stream_checksum_);
  payload["sessionId"] = session_id_;
  if (conversation_active_) payload["conversationSessionId"] = conversation_session_;
  payload["seq"] = sequence_;
  payload["bytes"] = pcm_bytes;
  payload["checksum"] = checksum(frame_checksum);
  payload["data"] = base64(pcm, pcm_bytes);
  if (!send("audio/chunk", doc)) {
    sendError("capture-backpressure", "audio transport queue rejected a PCM chunk");
    stopCapture("transport-backpressure");
    return;
  }
  stream_checksum_ = next_stream_checksum;
  ++sequence_;
  bytes_sent_ += pcm_bytes;
  if (millis() - last_status_ > 1000) { last_status_ = millis(); sendStatus("streaming"); }
}

