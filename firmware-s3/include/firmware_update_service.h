#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>

#include "protocol_transport.h"

// USB/WSS 共用的有界 ESP32-S3 应用 OTA 服务。
// 工厂镜像由桌面端提取应用段后，通过 firmware/* JSONL 指令下发。
class FirmwareUpdateService {
 public:
  FirmwareUpdateService();
  ~FirmwareUpdateService();

  void begin();
  bool handle(const char* topic, JsonObjectConst payload, JsonDocument& response,
              const char** response_topic);
  void abortForTransportChange();

  bool active() const { return active_; }
  bool rebootRequested() const { return reboot_requested_; }
  void clearRebootRequested() { reboot_requested_ = false; }

  const esp_partition_t* runningPartition() const;
  const esp_partition_t* nextPartition() const;
  uint64_t bootCount() const { return boot_count_; }
  const char* imageState() const;

 private:
  bool beginTransfer(JsonObjectConst payload, JsonObject response);
  bool appendChunk(JsonObjectConst payload, JsonObject response);
  bool commitTransfer(JsonObjectConst payload, JsonObject response);
  void sendStatus(JsonObjectConst payload, JsonDocument& response);
  void fail(JsonObject response, const char* code, const char* message);
  void resetTransfer();
  bool ownsCurrentTransport() const;
  bool parseSha256(const char* value);
  bool sha256Matches();
  void fillProgress(JsonObject response) const;

  bool active_ = false;
  bool reboot_requested_ = false;
  String transfer_id_;
  String expected_sha256_;
  String target_partition_;
  const esp_partition_t* target_ = nullptr;
  size_t total_bytes_ = 0;
  size_t received_bytes_ = 0;
  uint64_t next_sequence_ = 0;
  uint64_t boot_count_ = 0;
  TransportContext owner_;
  mbedtls_sha256_context sha256_;
  bool sha256_started_ = false;
};
