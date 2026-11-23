#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESP_I2S.h>

#include "audio_playback_stream.h"
#include "protocol_transport.h"
#include "transport_invariants.h"

class AudioService {
 public:
  void begin();
  void handle(const char* topic, JsonObjectConst payload);
  void poll();
  bool startCapture(const char* session_id = nullptr);
  void stopCapture(const char* reason = "stopped");
  void rejectCapture(const char* code, const char* message);
  void rejectPlayback(JsonObjectConst request, const char* message);
  void notifyEvent(const char* event);
  bool active() const { return active_; }
  bool available() const { return initialized_; }
  bool playbackAvailable() const { return playback_available_; }
  bool playbackActive() const { return playback_; }
  bool conversationActive() const { return conversation_active_; }
  uint8_t volume() const { return volume_; }
  void abortForTransportChange();

 private:
  bool configureCaptureCodec(bool include_aux);
  bool stopCaptureCodec();
  bool configurePlaybackCodec();
  bool startCaptureBus();
  bool startPlaybackBus();
  bool applyVolume(uint8_t volume);
  bool playEventCue(const char* event);
  void handleVolume(JsonObjectConst payload);
  void sendPlaybackState(JsonObjectConst request, bool ok, const char* error = nullptr,
                         size_t received_bytes = 0, size_t written_bytes = 0);
  bool writeRegister(uint8_t address, uint8_t reg, uint8_t value);
  bool updateRegister(uint8_t address, uint8_t reg, uint8_t mask, uint8_t value);
  bool send(const char* topic, JsonDocument& doc);
  void sendStatus(const char* reason);
  bool handleConversation(JsonObjectConst payload);
  bool suspendConversationCapture();
  void resumeConversationCapture();
  void sendError(const char* code, const char* message);
  void beginPlayback(JsonObjectConst payload);
  void writePlayback(JsonObjectConst payload);
  void failPlayback(JsonObjectConst request, const char* code, const char* message,
                    size_t declared_bytes = 0, size_t encoded_bytes = 0,
                    size_t decoded_bytes = 0, int decode_status = 0);
  void endPlayback(JsonObjectConst payload, bool play = true);
  void pollPlayback();
  void finishPlayback(bool ok, const char* error, bool played);
  void releasePlaybackBuffer();

  I2SClass i2s_;
  bool initialized_ = false;
  bool playback_available_ = false;
  bool active_ = false;
  String session_id_;
  uint32_t sequence_ = 0;
  uint32_t bytes_sent_ = 0;
  uint32_t capture_started_at_ = 0;
  uint64_t stream_checksum_ = 14695981039346656037ULL;
  uint32_t last_status_ = 0;
  uint32_t* capture_raw_ = nullptr;
  int16_t* capture_pcm_ = nullptr;
  bool playback_ = false;
  bool playback_bus_ = false;
  bool conversation_active_ = false;
  bool conversation_half_duplex_ = true;
  bool resume_capture_after_playback_ = false;
  String conversation_session_;
  String playback_session_;
  AudioPlaybackStream playback_stream_;
  uint8_t* playback_decode_ = nullptr;
  size_t playback_expected_bytes_ = 0;
  size_t playback_capacity_bytes_ = 0;
  size_t playback_buffered_bytes_ = 0;
  size_t playback_received_bytes_ = 0;
  s3transport::SequenceCounter playback_sequence_;
  bool playback_output_active_ = false;
  bool playback_end_received_ = false;
  size_t playback_written_bytes_ = 0;
  String playback_end_request_id_;
  TransportContext playback_owner_;
  uint8_t volume_ = 75;
  bool volume_persisted_ = false;
  bool capture_uses_mic3_ = false;
  uint8_t aux_probe_frames_ = 0;
  uint8_t aux_signal_frames_ = 0;
  String pending_event_cue_;
};

