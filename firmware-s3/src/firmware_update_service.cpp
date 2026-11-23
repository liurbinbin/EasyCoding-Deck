#include "firmware_update_service.h"

#include <Preferences.h>
#include <Update.h>
#include <esp_partition.h>
#include <mbedtls/base64.h>

#include <esp_heap_caps.h>

namespace {
constexpr size_t kMaxChunkBytes = 4092;
constexpr size_t kSha256HexLength = 64;
constexpr char kFirmwareProtocol[] = "esp-ota-json-v1";

String partitionLabel(const esp_partition_t* partition) {
  return partition ? String(partition->label) : String();
}

}  // namespace

namespace {
String digestToHex(const uint8_t digest[32]) {
  static constexpr char kHex[] = "0123456789abcdef";
  String result;
  result.reserve(kSha256HexLength);
  for (size_t index = 0; index < 32; ++index) {
    result += kHex[digest[index] >> 4];
    result += kHex[digest[index] & 0x0f];
  }
  return result;
}
}  // namespace

FirmwareUpdateService::FirmwareUpdateService() {
  mbedtls_sha256_init(&sha256_);
}

FirmwareUpdateService::~FirmwareUpdateService() {
  resetTransfer();
  mbedtls_sha256_free(&sha256_);
}

void FirmwareUpdateService::begin() {
  Preferences preferences;
  if (preferences.begin("firmware", false)) {
    boot_count_ = static_cast<uint64_t>(preferences.getUInt("bootCount", 0)) + 1;
    preferences.putUInt("bootCount", static_cast<uint32_t>(boot_count_));
    preferences.end();
  }
  const esp_partition_t* running = runningPartition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
      (state == ESP_OTA_IMG_NEW || state == ESP_OTA_IMG_PENDING_VERIFY)) {
    // 服务和传输已经初始化完成，确认新应用并关闭自动回滚窗口。
    esp_ota_mark_app_valid_cancel_rollback();
  }
}

const esp_partition_t* FirmwareUpdateService::runningPartition() const {
  return esp_ota_get_running_partition();
}

const esp_partition_t* FirmwareUpdateService::nextPartition() const {
  return esp_ota_get_next_update_partition(nullptr);
}

const char* FirmwareUpdateService::imageState() const {
  const esp_partition_t* partition = runningPartition();
  if (!partition) return "unknown";
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (esp_ota_get_state_partition(partition, &state) != ESP_OK) return "unknown";
  switch (state) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    default: return "undefined";
  }
}

bool FirmwareUpdateService::ownsCurrentTransport() const {
  const TransportContext owner = protocolTransport().owner();
  return active_ && owner_.valid && owner.sameConnection(owner_);
}

void FirmwareUpdateService::resetTransfer() {
  if (active_) Update.abort();
  active_ = false;
  transfer_id_ = "";
  expected_sha256_ = "";
  target_partition_ = "";
  target_ = nullptr;
  total_bytes_ = 0;
  received_bytes_ = 0;
  next_sequence_ = 0;
  owner_ = {};
  if (sha256_started_) {
    mbedtls_sha256_free(&sha256_);
    mbedtls_sha256_init(&sha256_);
    sha256_started_ = false;
  }
}

void FirmwareUpdateService::abortForTransportChange() {
  if (active_) resetTransfer();
}

void FirmwareUpdateService::fail(JsonObject response, const char* code,
                                 const char* message) {
  response["ok"] = false;
  response["code"] = code;
  response["error"] = message;
  response["message"] = message;
}

bool FirmwareUpdateService::parseSha256(const char* value) {
  if (!value || strlen(value) != kSha256HexLength) return false;
  for (size_t index = 0; index < kSha256HexLength; ++index) {
    const char c = value[index];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  expected_sha256_ = value;
  expected_sha256_.toLowerCase();
  return true;
}

bool FirmwareUpdateService::sha256Matches() {
  uint8_t digest[32] = {};
  if (mbedtls_sha256_finish(&sha256_, digest) != 0) return false;
  return expected_sha256_ == digestToHex(digest);
}

void FirmwareUpdateService::fillProgress(JsonObject response) const {
  response["transferId"] = transfer_id_;
  response["targetPartition"] = target_partition_;
  response["totalBytes"] = total_bytes_;
  response["receivedBytes"] = received_bytes_;
  response["nextSequence"] = next_sequence_;
}

bool FirmwareUpdateService::beginTransfer(JsonObjectConst payload,
                                           JsonObject response) {
  if (active_) {
    fail(response, "busy", "firmware transfer already active");
    return false;
  }
  if (protocolTransport().owner().kind != TransportKind::Usb) {
    fail(response, "usb_only", "firmware OTA is available only over USB");
    return false;
  }
  const char* transfer_id = payload["transferId"] | "";
  const uint64_t size = payload["size"] | 0ULL;
  const char* sha256 = payload["sha256"] | "";
  const esp_partition_t* next = nextPartition();
  if (!transfer_id || !*transfer_id || strlen(transfer_id) > 96 || size == 0 ||
      size > 0xffffffffULL || !next || size > next->size || !parseSha256(sha256)) {
    fail(response, "invalid_begin", "invalid transfer metadata or OTA partition capacity");
    return false;
  }
  if (Update.begin(static_cast<size_t>(size), U_FLASH) == false) {
    fail(response, "ota_begin_failed", "ESP32-S3 OTA partition could not be opened");
    return false;
  }
  if (mbedtls_sha256_starts(&sha256_, 0) != 0) {
    Update.abort();
    fail(response, "sha256_unavailable", "SHA-256 context could not be initialized");
    return false;
  }
  sha256_started_ = true;
  transfer_id_ = transfer_id;
  target_ = next;
  target_partition_ = partitionLabel(next);
  total_bytes_ = static_cast<size_t>(size);
  received_bytes_ = 0;
  next_sequence_ = 0;
  owner_ = protocolTransport().owner();
  active_ = true;
  response["ok"] = true;
  response["phase"] = "begin";
  fillProgress(response);
  return true;
}

bool FirmwareUpdateService::appendChunk(JsonObjectConst payload,
                                        JsonObject response) {
  const char* transfer_id = payload["transferId"] | "";
  const uint64_t sequence = payload["seq"] | UINT64_MAX;
  const uint32_t decoded_size = payload["decodedSize"] | 0;
  const char* encoded = payload["data"] | "";
  const size_t encoded_size = encoded ? strlen(encoded) : 0;
  response["phase"] = "chunk";
  response["transferId"] = transfer_id;
  response["seq"] = sequence;
  response["nextSequence"] = next_sequence_;
  response["receivedBytes"] = received_bytes_;
  response["chunkBytes"] = 0;
  response["duplicate"] = false;

  if (!active_ || !ownsCurrentTransport() || transfer_id_ != transfer_id) {
    fail(response, "not_active", "firmware transfer is not active for this connection");
    return false;
  }
  if (sequence != next_sequence_ || decoded_size == 0 || decoded_size > kMaxChunkBytes ||
      encoded_size != ((static_cast<size_t>(decoded_size) + 2) / 3) * 4 ||
      received_bytes_ + decoded_size > total_bytes_) {
    fail(response, "invalid_chunk", "firmware chunk sequence or size is invalid");
    return false;
  }

  uint8_t* decoded = static_cast<uint8_t*>(heap_caps_malloc(decoded_size, MALLOC_CAP_8BIT));
  size_t actual_size = decoded_size;
  const int decode_result = decoded
      ? mbedtls_base64_decode(decoded, decoded_size, &actual_size,
                              reinterpret_cast<const unsigned char*>(encoded), encoded_size)
      : -1;
  if (decode_result != 0 || actual_size != decoded_size) {
    if (decoded) heap_caps_free(decoded);
    response["nextSequence"] = next_sequence_;
    response["receivedBytes"] = received_bytes_;
    fail(response, "base64_mismatch", "firmware chunk base64 mismatch");
    return false;
  }
  const size_t written = Update.write(decoded, decoded_size);
  const int sha_result = written == decoded_size
      ? mbedtls_sha256_update(&sha256_, decoded, decoded_size) : -1;
  heap_caps_free(decoded);
  if (written != decoded_size || sha_result != 0) {
    // 写入结果未知时必须终止当前事务，避免桌面端把同一序号重放到
    // 可能已经部分写入的 OTA 槽；下一次 begin 会重新擦除并从头校验。
    resetTransfer();
    fail(response, "ota_write_failed", "ESP32-S3 OTA write failed; result is unknown");
    return false;
  }
  received_bytes_ += decoded_size;
  ++next_sequence_;
  response["ok"] = true;
  response["seq"] = sequence;
  response["nextSequence"] = next_sequence_;
  response["receivedBytes"] = received_bytes_;
  response["chunkBytes"] = decoded_size;
  return true;
}

bool FirmwareUpdateService::commitTransfer(JsonObjectConst payload,
                                           JsonObject response) {
  response["phase"] = "commit";
  response["transferId"] = payload["transferId"] | "";
  response["nextSequence"] = next_sequence_;
  response["receivedBytes"] = received_bytes_;
  if (!active_ || !ownsCurrentTransport() ||
      String(payload["transferId"] | "") != transfer_id_) {
    fail(response, "not_active", "firmware transfer is not active for this connection");
    return false;
  }
  if (received_bytes_ != total_bytes_ || !sha256Matches()) {
    resetTransfer();
    fail(response, "checksum_mismatch", "firmware image SHA-256 does not match");
    return false;
  }
  if (!Update.end(true) || !Update.isFinished() ||
      esp_ota_set_boot_partition(target_) != ESP_OK) {
    resetTransfer();
    fail(response, "ota_commit_failed", "ESP32-S3 OTA image could not be committed");
    return false;
  }
  response["ok"] = true;
  response["targetPartition"] = target_partition_;
  response["imageState"] = "pending_verify";
  active_ = false;
  reboot_requested_ = true;
  return true;
}

void FirmwareUpdateService::sendStatus(JsonObjectConst payload,
                                        JsonDocument& response) {
  JsonObject result = response.to<JsonObject>();
  result["requestId"] = payload["requestId"] | "";
  result["supported"] = true;
  result["protocol"] = kFirmwareProtocol;
  result["active"] = active_;
  result["imageState"] = imageState();
  const esp_partition_t* running = runningPartition();
  const esp_partition_t* next = nextPartition();
  result["runningPartition"]["label"] = partitionLabel(running);
  result["runningPartition"]["size"] = running ? running->size : 0;
  result["nextPartition"]["label"] = partitionLabel(next);
  result["nextPartition"]["size"] = next ? next->size : 0;
  result["chunkBytes"] = kMaxChunkBytes;
  if (active_) fillProgress(result);
}

bool FirmwareUpdateService::handle(const char* topic, JsonObjectConst payload,
                                   JsonDocument& response,
                                   const char** response_topic) {
  if (!topic || strncmp(topic, "firmware/", 9) != 0) return false;
  *response_topic = !strcmp(topic, "firmware/query") ? "firmware/status" : "firmware/ack";
  response.clear();
  if (!strcmp(topic, "firmware/query")) {
    sendStatus(payload, response);
    return true;
  }
  if (!protocolTransport().admitted(protocolTransport().owner())) return false;
  if (!strcmp(topic, "firmware/begin")) {
    response["phase"] = "begin";
    beginTransfer(payload, response.to<JsonObject>());
    return true;
  }
  if (!strcmp(topic, "firmware/chunk")) {
    appendChunk(payload, response.to<JsonObject>());
    return true;
  }
  if (!strcmp(topic, "firmware/commit")) {
    commitTransfer(payload, response.to<JsonObject>());
    return true;
  }
  fail(response.to<JsonObject>(), "unknown_command", "unknown firmware command");
  return true;
}
