#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "protocol_transport.h"

class WifiDeviceTransport final : public TransportAdapter {
 public:
  static constexpr uint16_t kSecurePort = 8443;

  TransportKind kind() const override { return TransportKind::Wifi; }
  void begin(ProtocolTransport& transport) override;
  void poll() override;
  TransportSendResult sendText(const TransportContext& target,
                               const String& message) override;
  void closePeer(uint32_t peer_id) override;
  void flush() override {}

  void setIdentity(const String& board_device_id, const String& boot_id);
  bool handleControl(const char* topic, JsonObjectConst payload,
                     const TransportContext& source, bool& authenticated_now);
  void sendStatus(const TransportContext& target, const char* request_id = "");

  bool configured() const { return configured_; }
  bool connected() const { return WiFi.status() == WL_CONNECTED; }
  bool serverRunning() const { return server_ != nullptr; }
  bool tlsReady() const { return !tls_cert_.isEmpty() && !tls_key_.isEmpty(); }
  const String& tlsFingerprint() const { return tls_fingerprint_; }
  const char* stateName() const;

 private:
  struct Session {
    int fd = -1;
    uint32_t epoch = 0;
    uint32_t generation = 0;
    bool authenticated = false;
    bool challenge_pending = false;
    bool closed = false;
    uint8_t failures = 0;
    uint8_t sends_inflight = 0;
    uint32_t connected_at = 0;
    uint32_t challenge_deadline = 0;
    uint32_t last_rx = 0;
    String nonce;
    String fragments;
  };

  struct SessionContext {
    WifiDeviceTransport* owner = nullptr;
    int fd = -1;
    uint32_t epoch = 0;
  };

  struct OutboundFrame {
    WifiDeviceTransport* owner = nullptr;
    int fd = -1;
    uint32_t epoch = 0;
    String text;
    httpd_ws_frame_t frame{};
  };

  struct PendingFrame {
    int fd = -1;
    uint32_t epoch = 0;
    String text;
  };

  struct ProvisioningJob {
    WifiDeviceTransport* owner = nullptr;
    TransportContext target;
    String request_id;
    String ssid;
    String password;
    String pair_secret;
    String certificate;
    String private_key;
    String fingerprint;
    uint32_t stack_min_free_bytes = 0;
    bool generated = false;
  };

  static esp_err_t websocketHandler(httpd_req_t* request);
  static void sessionFree(void* context);
  static void sendComplete(esp_err_t error, int fd, void* context);
  static void tlsIdentityTask(void* context);
  esp_err_t handleWebsocket(httpd_req_t* request);
  bool enqueueFrame(int fd, uint32_t epoch, const String& text);
  bool takeFrame(PendingFrame& frame);
  void processPendingFrames();
  void processSessionLifecycle();
  void sendChallenge();
  void markSessionClosed(int fd, uint32_t epoch);
  void completeSend(int fd, uint32_t epoch, esp_err_t error);
  TransportContext sessionContext(bool authenticated_override = false,
                                  bool allow_closed = false) const;

  bool loadConfiguration();
  bool persistConfiguration(const String& ssid, const String& password,
                            const String& pair_secret);
  void clearConfiguration();
  bool startTlsProvisioning(const TransportContext& target,
                            const String& request_id, const String& ssid,
                            const String& password, const String& pair_secret);
  void pollTlsProvisioning();
  bool generateTlsIdentity(String& certificate, String& private_key,
                           String& fingerprint);
  bool startServer();
  void stopServer();
  void startMdns();
  void stopMdns();
  void startConnection(bool immediate = false);
  void pollConnection();
  void pollScan();
  void beginScan(JsonObjectConst payload, const TransportContext& source);
  void configure(JsonObjectConst payload, const TransportContext& source);
  void forget(JsonObjectConst payload, const TransportContext& source);
  bool authenticate(JsonObjectConst payload, const TransportContext& source,
                    bool& authenticated_now);
  bool verifyProof(const String& client_id, const String& nonce,
                   const String& proof) const;
  void sendConfigAck(const TransportContext& target, const char* request_id,
                     bool ok, const char* code, const char* message);
  void sendAuthAck(const TransportContext& target, const char* request_id,
                   bool ok, const char* code);
  void zeroizeSecrets();

  ProtocolTransport* transport_ = nullptr;
  String board_device_id_;
  String boot_id_;
  String ssid_;
  String password_;
  String pair_secret_;
  String tls_cert_;
  String tls_key_;
  String tls_fingerprint_;
  bool configured_ = false;
  bool connecting_ = false;
  bool mdns_running_ = false;
  uint32_t connect_started_at_ = 0;
  uint32_t next_connect_at_ = 0;
  uint32_t reconnect_delay_ms_ = 1000;

  httpd_handle_t server_ = nullptr;
  Session session_;
  uint32_t next_session_epoch_ = 1;
  SemaphoreHandle_t session_mutex_ = nullptr;
  QueueHandle_t provisioning_results_ = nullptr;
  bool provisioning_active_ = false;
  uint32_t tls_identity_stack_min_free_bytes_ = 0;
  PendingFrame pending_[16];
  size_t pending_head_ = 0;
  size_t pending_count_ = 0;

  bool scan_active_ = false;
  String scan_request_id_;
  TransportContext scan_target_;
};
