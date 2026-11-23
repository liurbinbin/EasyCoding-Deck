#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "protocol_transport.h"

class AvatarService {
 public:
  ~AvatarService();

  void begin();
  bool handle(const char* topic, JsonObjectConst payload, JsonDocument& ack);
  bool poll();
  bool selectState(const char* state);
  bool selectNextState();
  bool active() const { return active_ != nullptr && frame_count_ > 0; }
  const uint16_t* frame() const;
  uint8_t frameCount() const { return frame_count_; }
  uint8_t stateCount() const { return state_count_; }
  uint16_t frameDurationMs() const { return frame_duration_ms_; }
  bool transferActive() const { return staging_ != nullptr; }
  void abortForTransportChange();

 private:
  bool beginTransfer(JsonObjectConst payload, JsonObject ack);
  bool appendChunk(JsonObjectConst payload, JsonObject ack);
  bool commitTransfer(JsonObjectConst payload, JsonObject ack);
  bool clear(JsonObject ack);
  void discardStaging();
  void fail(JsonObject ack, const char* code, const String& message);
  bool loadPersisted();
  bool persistStaging();

  struct StateEntry {
    char name[16] = {};
    uint32_t offset = 0;
    uint8_t frame_count = 0;
    uint16_t frame_duration_ms = 167;
  };

  uint8_t* active_ = nullptr;
  uint8_t* staging_ = nullptr;
  size_t staging_size_ = 0;
  size_t received_ = 0;
  uint64_t expected_checksum_ = 0;
  uint64_t running_checksum_ = 14695981039346656037ULL;
  uint8_t pending_frame_count_ = 0;
  uint16_t pending_frame_duration_ms_ = 167;
  StateEntry pending_states_[14] = {};
  uint8_t pending_state_count_ = 0;
  StateEntry states_[14] = {};
  uint8_t state_count_ = 0;
  uint8_t state_index_ = 0;
  uint8_t frame_count_ = 0;
  uint8_t frame_index_ = 0;
  uint16_t frame_duration_ms_ = 167;
  uint32_t next_frame_at_ = 0;
  TransportContext staging_owner_;
};
