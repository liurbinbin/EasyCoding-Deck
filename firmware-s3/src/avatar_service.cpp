#include "avatar_service.h"

#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>

namespace {
constexpr size_t kFrameBytes = 128U * 128U * 2U;
constexpr uint8_t kMaxStates = 14;
constexpr uint8_t kMaxFramesPerState = 84;
constexpr uint8_t kMaxTotalFrames = 84;
constexpr size_t kMaxBytes = kFrameBytes * kMaxTotalFrames;
constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;
constexpr uint32_t kFileMagic = 0x33564148;
constexpr char kAvatarPath[] = "/avatar-v2.bin";
constexpr char kAvatarTempPath[] = "/avatar-v2.tmp";
constexpr char kAvatarBackupPath[] = "/avatar-v2.bak";

struct PersistedState {
  char name[16];
  uint32_t offset;
  uint8_t frame_count;
  uint8_t reserved;
  uint16_t frame_duration_ms;
};

struct PersistedHeader {
  uint32_t magic;
  uint16_t version;
  uint8_t state_count;
  uint8_t reserved;
  uint32_t total_bytes;
  uint64_t checksum;
  PersistedState states[kMaxStates];
};

String boardDeviceId() {
  return String("s3-tkeyboard-") + String((uint32_t)ESP.getEfuseMac(), HEX);
}

bool parseChecksum(const char* value, uint64_t& result) {
  if (!value || strlen(value) != 16) return false;
  char* end = nullptr;
  result = strtoull(value, &end, 16);
  return end && *end == '\0';
}

bool validStateName(const char* name) {
  if (!name || !*name || strlen(name) >= 16) return false;
  for (const char* cursor = name; *cursor; ++cursor) {
    if (!((*cursor >= 'a' && *cursor <= 'z') || *cursor == '_')) return false;
  }
  return true;
}
}

AvatarService::~AvatarService() {
  discardStaging();
  if (active_) heap_caps_free(active_);
}

void AvatarService::begin() {
  if (!LittleFS.begin(true)) return;
  if (!LittleFS.exists(kAvatarPath) && LittleFS.exists(kAvatarBackupPath)) {
    LittleFS.rename(kAvatarBackupPath, kAvatarPath);
  }
  LittleFS.remove(kAvatarTempPath);
  if (LittleFS.exists(kAvatarPath)) LittleFS.remove(kAvatarBackupPath);
  loadPersisted();
}

void AvatarService::fail(JsonObject ack, const char* code, const String& message) {
  ack["ok"] = false;
  ack["code"] = code;
  ack["message"] = message;
}

void AvatarService::discardStaging() {
  if (staging_) heap_caps_free(staging_);
  staging_ = nullptr;
  staging_size_ = 0;
  received_ = 0;
  pending_frame_count_ = 0;
  pending_state_count_ = 0;
  running_checksum_ = kFnvOffset;
  staging_owner_ = {};
}

void AvatarService::abortForTransportChange() {
  discardStaging();
}

bool AvatarService::beginTransfer(JsonObjectConst payload, JsonObject ack) {
  discardStaging();
  const uint32_t width = payload["width"] | 0;
  const uint32_t height = payload["height"] | 0;
  const char* format = payload["format"] | "";
  const uint32_t frame_bytes = payload["frameBytes"] | 0;
  const uint32_t total_bytes = payload["totalBytes"] | 0;
  if (width != 128 || height != 128 || strcmp(format, "rgb565le") ||
      frame_bytes != kFrameBytes || total_bytes == 0 || total_bytes > kMaxBytes ||
      total_bytes % kFrameBytes != 0) {
    fail(ack, "invalid_avatar", "expected bounded 128x128 rgb565le frames");
    return false;
  }
  if (!parseChecksum(payload["checksum"] | "", expected_checksum_)) {
    fail(ack, "invalid_checksum", "checksum must be 16 hexadecimal characters");
    return false;
  }
  LittleFS.remove(kAvatarTempPath);
  const size_t free_bytes = LittleFS.totalBytes() - LittleFS.usedBytes();
  const size_t required_bytes = total_bytes + sizeof(PersistedHeader) + 64U * 1024U;
  if (free_bytes < required_bytes) {
    fail(ack, "storage_capacity", "not enough LittleFS space for atomic avatar update");
    ack["requiredBytes"] = required_bytes;
    ack["freeBytes"] = free_bytes;
    return false;
  }

  JsonArrayConst state_list = payload["states"].as<JsonArrayConst>();
  if (!state_list.isNull()) {
    if (state_list.size() == 0 || state_list.size() > kMaxStates) {
      fail(ack, "invalid_states", "avatar package must contain 1-14 states");
      return false;
    }
    uint32_t covered_bytes = 0;
    for (JsonObjectConst item : state_list) {
      const char* name = item["name"] | "";
      const uint32_t offset = item["offset"] | UINT32_MAX;
      const uint32_t frames = item["frameCount"] | 0;
      const uint32_t duration = item["frameDurationMs"] | 0;
      if (!validStateName(name) || offset != covered_bytes || frames == 0 ||
          frames > kMaxFramesPerState || duration == 0 || duration > 10000) {
        fail(ack, "invalid_states", "invalid or non-contiguous avatar state directory");
        return false;
      }
      for (uint8_t index = 0; index < pending_state_count_; ++index) {
        if (!strcmp(name, pending_states_[index].name)) {
          fail(ack, "invalid_states", "duplicate avatar state name");
          return false;
        }
      }
      StateEntry& state = pending_states_[pending_state_count_++];
      strlcpy(state.name, name, sizeof(state.name));
      state.offset = offset;
      state.frame_count = static_cast<uint8_t>(frames);
      state.frame_duration_ms = static_cast<uint16_t>(duration);
      covered_bytes += frames * kFrameBytes;
    }
    if (covered_bytes != total_bytes) {
      fail(ack, "invalid_states", "avatar state directory does not cover payload");
      return false;
    }
  } else {
    const uint32_t frames = payload["frameCount"] | 0;
    const uint32_t duration = payload["frameDurationMs"] | 0;
    if (frames == 0 || frames > kMaxFramesPerState || total_bytes != frames * kFrameBytes ||
        duration == 0 || duration > 10000) {
      fail(ack, "invalid_avatar", "legacy avatar exceeds the bounded frame directory");
      return false;
    }
    strlcpy(pending_states_[0].name, "idle", sizeof(pending_states_[0].name));
    pending_states_[0].offset = 0;
    pending_states_[0].frame_count = static_cast<uint8_t>(frames);
    pending_states_[0].frame_duration_ms = static_cast<uint16_t>(duration);
    pending_state_count_ = 1;
  }

  staging_ = static_cast<uint8_t*>(heap_caps_malloc(total_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!staging_) {
    fail(ack, "no_psram", "avatar PSRAM allocation failed");
    return false;
  }
  staging_size_ = total_bytes;
  staging_owner_ = protocolTransport().owner();
  running_checksum_ = kFnvOffset;
  ack["ok"] = true;
  ack["receivedBytes"] = 0;
  ack["totalBytes"] = total_bytes;
  ack["stateCount"] = pending_state_count_;
  return true;
}

bool AvatarService::appendChunk(JsonObjectConst payload, JsonObject ack) {
  if (!staging_) {
    fail(ack, "no_transfer", "avatar transfer has not started");
    return false;
  }
  if (!staging_owner_.sameConnection(protocolTransport().owner())) {
    fail(ack, "transfer_owner_changed", "avatar transfer belongs to a retired connection");
    discardStaging();
    return false;
  }
  const size_t offset = payload["offset"] | SIZE_MAX;
  const size_t declared = payload["bytes"] | 0;
  const char* encoded = payload["data"] | "";
  if (offset != received_ || declared == 0 || declared > 4096 || received_ + declared > staging_size_) {
    fail(ack, "invalid_offset", String("expected offset ") + received_);
    return false;
  }
  size_t decoded = 0;
  const int status = mbedtls_base64_decode(staging_ + received_, declared, &decoded,
                                            reinterpret_cast<const unsigned char*>(encoded),
                                            strlen(encoded));
  if (status != 0 || decoded != declared) {
    fail(ack, "invalid_chunk", "avatar base64 chunk length mismatch");
    return false;
  }
  for (size_t i = 0; i < decoded; ++i) {
    running_checksum_ ^= staging_[received_ + i];
    running_checksum_ *= kFnvPrime;
  }
  received_ += decoded;
  ack["ok"] = true;
  ack["receivedBytes"] = received_;
  ack["totalBytes"] = staging_size_;
  return true;
}

bool AvatarService::persistStaging() {
  PersistedHeader header{};
  header.magic = kFileMagic;
  header.version = 3;
  header.state_count = pending_state_count_;
  header.total_bytes = staging_size_;
  header.checksum = expected_checksum_;
  for (uint8_t index = 0; index < pending_state_count_; ++index) {
    strlcpy(header.states[index].name, pending_states_[index].name,
            sizeof(header.states[index].name));
    header.states[index].offset = pending_states_[index].offset;
    header.states[index].frame_count = pending_states_[index].frame_count;
    header.states[index].frame_duration_ms = pending_states_[index].frame_duration_ms;
  }
  LittleFS.remove(kAvatarTempPath);
  File output = LittleFS.open(kAvatarTempPath, FILE_WRITE);
  if (!output) return false;
  const bool written = output.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) == sizeof(header) &&
                       output.write(staging_, staging_size_) == staging_size_;
  output.flush();
  output.close();
  if (!written) {
    LittleFS.remove(kAvatarTempPath);
    return false;
  }
  LittleFS.remove(kAvatarBackupPath);
  const bool had_active = LittleFS.exists(kAvatarPath);
  if (had_active && !LittleFS.rename(kAvatarPath, kAvatarBackupPath)) {
    LittleFS.remove(kAvatarTempPath);
    return false;
  }
  if (!LittleFS.rename(kAvatarTempPath, kAvatarPath)) {
    if (had_active) LittleFS.rename(kAvatarBackupPath, kAvatarPath);
    return false;
  }
  LittleFS.remove(kAvatarBackupPath);
  return true;
}

bool AvatarService::commitTransfer(JsonObjectConst payload, JsonObject ack) {
  if (staging_ && !staging_owner_.sameConnection(protocolTransport().owner())) {
    fail(ack, "transfer_owner_changed", "avatar transfer belongs to a retired connection");
    discardStaging();
    return false;
  }
  uint64_t requested_checksum = 0;
  const size_t total = payload["totalBytes"] | 0;
  if (!staging_ || !parseChecksum(payload["checksum"] | "", requested_checksum) ||
      total != staging_size_ || received_ != staging_size_ ||
      requested_checksum != expected_checksum_ || running_checksum_ != expected_checksum_) {
    fail(ack, "integrity_failed", "avatar size or checksum mismatch");
    discardStaging();
    return false;
  }
  if (!persistStaging()) {
    fail(ack, "storage_failed", "avatar Flash persistence failed; previous avatar retained");
    discardStaging();
    return false;
  }
  if (active_) heap_caps_free(active_);
  active_ = staging_;
  staging_ = nullptr;
  staging_owner_ = {};
  memcpy(states_, pending_states_, sizeof(states_));
  state_count_ = pending_state_count_;
  state_index_ = 0;
  frame_index_ = 0;
  frame_count_ = states_[0].frame_count;
  frame_duration_ms_ = states_[0].frame_duration_ms;
  next_frame_at_ = millis() + frame_duration_ms_;
  staging_size_ = 0;
  received_ = 0;
  pending_state_count_ = 0;
  ack["ok"] = true;
  ack["active"] = true;
  ack["frameCount"] = frame_count_;
  ack["stateCount"] = state_count_;
  ack["persisted"] = true;
  ack["totalBytes"] = total;
  return true;
}

bool AvatarService::loadPersisted() {
  File input = LittleFS.open(kAvatarPath, FILE_READ);
  if (!input) return false;
  PersistedHeader header{};
  if (input.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header) ||
      header.magic != kFileMagic || header.version != 3 || header.state_count == 0 ||
      header.state_count > kMaxStates || header.total_bytes == 0 || header.total_bytes > kMaxBytes ||
      input.size() != sizeof(header) + header.total_bytes) {
    input.close();
    return false;
  }
  uint8_t* data = static_cast<uint8_t*>(heap_caps_malloc(header.total_bytes,
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!data) {
    input.close();
    return false;
  }
  if (input.read(data, header.total_bytes) != header.total_bytes) {
    input.close();
    heap_caps_free(data);
    return false;
  }
  input.close();
  uint64_t checksum = kFnvOffset;
  for (size_t index = 0; index < header.total_bytes; ++index) {
    checksum ^= data[index];
    checksum *= kFnvPrime;
  }
  uint32_t covered = 0;
  bool valid = checksum == header.checksum;
  for (uint8_t index = 0; valid && index < header.state_count; ++index) {
    const PersistedState& stored = header.states[index];
    valid = validStateName(stored.name) && stored.offset == covered && stored.frame_count > 0 &&
            stored.frame_count <= kMaxFramesPerState && stored.frame_duration_ms > 0 &&
            stored.frame_duration_ms <= 10000;
    covered += stored.frame_count * kFrameBytes;
  }
  if (!valid || covered != header.total_bytes) {
    heap_caps_free(data);
    return false;
  }
  if (active_) heap_caps_free(active_);
  active_ = data;
  state_count_ = header.state_count;
  for (uint8_t index = 0; index < state_count_; ++index) {
    strlcpy(states_[index].name, header.states[index].name, sizeof(states_[index].name));
    states_[index].offset = header.states[index].offset;
    states_[index].frame_count = header.states[index].frame_count;
    states_[index].frame_duration_ms = header.states[index].frame_duration_ms;
  }
  state_index_ = 0;
  frame_index_ = 0;
  frame_count_ = states_[0].frame_count;
  frame_duration_ms_ = states_[0].frame_duration_ms;
  next_frame_at_ = millis() + frame_duration_ms_;
  return true;
}

bool AvatarService::clear(JsonObject ack) {
  discardStaging();
  if (active_) heap_caps_free(active_);
  active_ = nullptr;
  state_count_ = 0;
  frame_count_ = 0;
  frame_index_ = 0;
  LittleFS.remove(kAvatarPath);
  LittleFS.remove(kAvatarTempPath);
  LittleFS.remove(kAvatarBackupPath);
  ack["ok"] = true;
  ack["active"] = false;
  ack["persisted"] = false;
  return true;
}

bool AvatarService::handle(const char* topic, JsonObjectConst payload, JsonDocument& response) {
  if (strncmp(topic, "display/avatar_", 15) != 0) return false;
  JsonObject ack = response.to<JsonObject>();
  ack["requestId"] = payload["requestId"] | "";
  ack["requestTopic"] = topic;
  ack["boardDeviceId"] = boardDeviceId();
  if (!strcmp(topic, "display/avatar_begin")) beginTransfer(payload, ack);
  else if (!strcmp(topic, "display/avatar_chunk")) appendChunk(payload, ack);
  else if (!strcmp(topic, "display/avatar_end")) commitTransfer(payload, ack);
  else if (!strcmp(topic, "display/avatar_clear")) clear(ack);
  else fail(ack, "not_implemented", "unknown avatar command");
  return true;
}

bool AvatarService::selectState(const char* requested) {
  if (!active() || !requested || !*requested) return false;
  String normalized(requested);
  normalized.toLowerCase();
  normalized.replace('-', '_');
  if (normalized == "thinking" || normalized == "tool_running") normalized = "working";
  else if (normalized == "waiting_user" || normalized == "needs_user") normalized = "waiting";
  else if (normalized == "complete" || normalized == "completed") normalized = "done";
  else if (normalized == "failed") normalized = "error";
  uint8_t selected = 0;
  bool found = false;
  for (uint8_t index = 0; index < state_count_; ++index) {
    if (normalized == states_[index].name) {
      selected = index;
      found = true;
      break;
    }
  }
  if (!found) {
    for (uint8_t index = 0; index < state_count_; ++index) {
      if (!strcmp(states_[index].name, "idle")) {
        selected = index;
        break;
      }
    }
  }
  if (selected == state_index_) return false;
  state_index_ = selected;
  frame_index_ = 0;
  frame_count_ = states_[selected].frame_count;
  frame_duration_ms_ = states_[selected].frame_duration_ms;
  next_frame_at_ = millis() + frame_duration_ms_;
  return true;
}

bool AvatarService::selectNextState() {
  if (!active() || state_count_ < 2) return false;
  state_index_ = static_cast<uint8_t>((state_index_ + 1) % state_count_);
  frame_index_ = 0;
  frame_count_ = states_[state_index_].frame_count;
  frame_duration_ms_ = states_[state_index_].frame_duration_ms;
  next_frame_at_ = millis() + frame_duration_ms_;
  return true;
}

bool AvatarService::poll() {
  if (!active() || frame_count_ < 2 || static_cast<int32_t>(millis() - next_frame_at_) < 0)
    return false;
  frame_index_ = static_cast<uint8_t>((frame_index_ + 1) % frame_count_);
  next_frame_at_ = millis() + frame_duration_ms_;
  return true;
}

const uint16_t* AvatarService::frame() const {
  if (!active()) return nullptr;
  return reinterpret_cast<const uint16_t*>(active_ + states_[state_index_].offset +
                                           static_cast<size_t>(frame_index_) * kFrameBytes);
}
