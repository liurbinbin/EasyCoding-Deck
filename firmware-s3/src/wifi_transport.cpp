#include "wifi_transport.h"
#include "transport_invariants.h"

#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_https_server.h>
#include <esp_system.h>
#include <freertos/task.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace {
constexpr char kNamespace[] = "s3-wifi";
constexpr char kSsidKey[] = "ssid";
constexpr char kPasswordKey[] = "password";
constexpr char kPairSecretKey[] = "pair-secret";
constexpr char kCertificateKey[] = "tls-cert";
constexpr char kPrivateKeyKey[] = "tls-key";
constexpr char kFingerprintKey[] = "tls-fp";
constexpr char kConfiguredKey[] = "configured";
constexpr char kServiceType[] = "hachimodock-s3";
constexpr char kServiceProtocol[] = "tcp";
constexpr char kWebsocketPath[] = "/device/v1";
constexpr char kWebsocketSubprotocol[] = "hachimodock.s3.v1";
constexpr uint32_t kConnectTimeoutMs = 20000;
constexpr uint32_t kMaxReconnectDelayMs = 30000;
constexpr uint32_t kChallengeLifetimeMs = 10000;
constexpr uint32_t kAuthenticatedLeaseMs = 120000;
constexpr size_t kMaxClientIdLength = 63;
constexpr size_t kMaxNetworks = 20;
constexpr size_t kTlsCertificateBufferBytes = 2048;
constexpr size_t kTlsPrivateKeyBufferBytes = 1024;
constexpr uint32_t kTlsIdentityTaskStackBytes = 24 * 1024;
static_assert(kTlsIdentityTaskStackBytes >= 16 * 1024,
              "TLS identity task requires dedicated stack headroom");

struct TlsIdentityContexts {
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context random;
  mbedtls_pk_context key;
  mbedtls_x509write_cert certificate_writer;
  mbedtls_x509_crt parsed;
};

bool validHex(const String& value, size_t expected_length) {
  return s3transport::isHex(value.c_str(), value.length(), expected_length);
}

bool decodeHex(const String& value, uint8_t* output, size_t output_length) {
  if (!output || value.length() != output_length * 2) return false;
  auto nibble = [](char character) -> int {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
  };
  for (size_t index = 0; index < output_length; ++index) {
    const int high = nibble(value[index * 2]);
    const int low = nibble(value[index * 2 + 1]);
    if (high < 0 || low < 0) return false;
    output[index] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

String encodeHex(const uint8_t* input, size_t length) {
  static constexpr char kHex[] = "0123456789abcdef";
  String result;
  result.reserve(length * 2);
  for (size_t index = 0; index < length; ++index) {
    result += kHex[input[index] >> 4];
    result += kHex[input[index] & 0x0f];
  }
  return result;
}

String randomHex(size_t bytes) {
  String result;
  result.reserve(bytes * 2);
  while (bytes > 0) {
    const uint32_t value = esp_random();
    const size_t take = std::min(bytes, sizeof(value));
    result += encodeHex(reinterpret_cast<const uint8_t*>(&value), take);
    bytes -= take;
  }
  return result;
}

bool elapsed(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

void zeroizeString(String& value) {
  for (size_t index = 0; index < value.length(); ++index)
    value.setCharAt(index, '\0');
  value = "";
}

uint8_t* allocateCryptoBuffer(size_t bytes) {
  auto* buffer = static_cast<uint8_t*>(heap_caps_calloc(
      1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!buffer) {
    buffer = static_cast<uint8_t*>(heap_caps_calloc(
        1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  return buffer;
}

const char* authModeName(wifi_auth_mode_t mode) {
  switch (mode) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "wep";
    case WIFI_AUTH_WPA_PSK: return "wpa-psk";
    case WIFI_AUTH_WPA2_PSK: return "wpa2-psk";
    case WIFI_AUTH_WPA_WPA2_PSK: return "wpa-wpa2-psk";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "wpa2-enterprise";
    case WIFI_AUTH_WPA3_PSK: return "wpa3-psk";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "wpa2-wpa3-psk";
    default: return "unknown";
  }
}
}

void WifiDeviceTransport::setIdentity(const String& board_device_id,
                                      const String& boot_id) {
  board_device_id_ = board_device_id;
  boot_id_ = boot_id;
}

void WifiDeviceTransport::begin(ProtocolTransport& transport) {
  session_mutex_ = xSemaphoreCreateMutex();
  provisioning_results_ = xQueueCreate(1, sizeof(ProvisioningJob*));
  if (!session_mutex_ || !provisioning_results_) {
    if (session_mutex_) vSemaphoreDelete(session_mutex_);
    if (provisioning_results_) vQueueDelete(provisioning_results_);
    session_mutex_ = nullptr;
    provisioning_results_ = nullptr;
    return;
  }
  transport_ = &transport;
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  if (loadConfiguration()) startConnection(true);
}

bool WifiDeviceTransport::loadConfiguration() {
  Preferences preferences;
  if (!preferences.begin(kNamespace, true)) return false;
  configured_ = preferences.getBool(kConfiguredKey, false);
  ssid_ = preferences.getString(kSsidKey, "");
  password_ = preferences.getString(kPasswordKey, "");
  pair_secret_ = preferences.getString(kPairSecretKey, "");
  tls_cert_ = preferences.getString(kCertificateKey, "");
  tls_key_ = preferences.getString(kPrivateKeyKey, "");
  tls_fingerprint_ = preferences.getString(kFingerprintKey, "");
  preferences.end();
  configured_ = configured_ && !ssid_.isEmpty() && validHex(pair_secret_, 64) &&
                !tls_cert_.isEmpty() && !tls_key_.isEmpty() &&
                validHex(tls_fingerprint_, 64);
  return configured_;
}

bool WifiDeviceTransport::persistConfiguration(const String& ssid,
                                               const String& password,
                                               const String& pair_secret) {
  Preferences preferences;
  if (!preferences.begin(kNamespace, false)) return false;
  preferences.putBool(kConfiguredKey, false);
  const bool ssid_ok = preferences.putString(kSsidKey, ssid) == ssid.length();
  const size_t password_written = preferences.putString(kPasswordKey, password);
  const bool password_ok = password.isEmpty()
      ? preferences.getString(kPasswordKey, "\x01") == password
      : password_written == password.length();
  const bool ok = ssid_ok && password_ok &&
                  preferences.putString(kPairSecretKey, pair_secret) == pair_secret.length() &&
                  preferences.putString(kCertificateKey, tls_cert_) == tls_cert_.length() &&
                  preferences.putString(kPrivateKeyKey, tls_key_) == tls_key_.length() &&
                  preferences.putString(kFingerprintKey, tls_fingerprint_) ==
                      tls_fingerprint_.length() &&
                  preferences.putBool(kConfiguredKey, true);
  if (!ok) preferences.putBool(kConfiguredKey, false);
  preferences.end();
  if (!ok) return false;
  ssid_ = ssid;
  password_ = password;
  pair_secret_ = pair_secret;
  configured_ = true;
  return true;
}

void WifiDeviceTransport::zeroizeSecrets() {
  zeroizeString(password_);
  zeroizeString(pair_secret_);
  zeroizeString(tls_key_);
}

void WifiDeviceTransport::clearConfiguration() {
  stopServer();
  stopMdns();
  WiFi.disconnect(true, true);
  Preferences preferences;
  if (preferences.begin(kNamespace, false)) {
    preferences.clear();
    preferences.end();
  }
  zeroizeSecrets();
  ssid_ = "";
  tls_cert_ = "";
  tls_fingerprint_ = "";
  configured_ = false;
  connecting_ = false;
  next_connect_at_ = 0;
}

bool WifiDeviceTransport::generateTlsIdentity(String& certificate,
                                              String& private_key,
                                              String& fingerprint) {
  certificate = "";
  zeroizeString(private_key);
  fingerprint = "";

  auto* contexts = new (std::nothrow) TlsIdentityContexts{};
  uint8_t* certificate_buffer =
      allocateCryptoBuffer(kTlsCertificateBufferBytes);
  uint8_t* key_buffer = allocateCryptoBuffer(kTlsPrivateKeyBufferBytes);
  if (!contexts || !certificate_buffer || !key_buffer) {
    if (key_buffer) {
      mbedtls_platform_zeroize(key_buffer, kTlsPrivateKeyBufferBytes);
      heap_caps_free(key_buffer);
    }
    if (certificate_buffer) heap_caps_free(certificate_buffer);
    delete contexts;
    return false;
  }

  mbedtls_entropy_init(&contexts->entropy);
  mbedtls_ctr_drbg_init(&contexts->random);
  mbedtls_pk_init(&contexts->key);
  mbedtls_x509write_crt_init(&contexts->certificate_writer);
  mbedtls_x509_crt_init(&contexts->parsed);

  uint8_t serial_bytes[16] = {};
  uint8_t digest[32] = {};
  const char personalization[] = "hachimodock-s3-device-identity";
  String distinguished_name =
      String("CN=") + board_device_id_ + ",O=HachimoDock";

  bool ok = mbedtls_ctr_drbg_seed(
      &contexts->random, mbedtls_entropy_func, &contexts->entropy,
      reinterpret_cast<const unsigned char*>(personalization),
      sizeof(personalization) - 1) == 0;
  ok = ok && mbedtls_pk_setup(
                 &contexts->key,
                 mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0;
  ok = ok && mbedtls_ecp_gen_key(
                 MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(contexts->key),
                 mbedtls_ctr_drbg_random, &contexts->random) == 0;
  ok = ok && mbedtls_ctr_drbg_random(
                 &contexts->random, serial_bytes, sizeof(serial_bytes)) == 0;
  if (ok) {
    mbedtls_x509write_crt_set_version(
        &contexts->certificate_writer, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(
        &contexts->certificate_writer, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(
        &contexts->certificate_writer, &contexts->key);
    mbedtls_x509write_crt_set_issuer_key(
        &contexts->certificate_writer, &contexts->key);
    ok = mbedtls_x509write_crt_set_subject_name(
             &contexts->certificate_writer, distinguished_name.c_str()) == 0 &&
         mbedtls_x509write_crt_set_issuer_name(
             &contexts->certificate_writer, distinguished_name.c_str()) == 0 &&
         mbedtls_x509write_crt_set_serial_raw(
             &contexts->certificate_writer, serial_bytes,
             sizeof(serial_bytes)) == 0 &&
         mbedtls_x509write_crt_set_validity(
             &contexts->certificate_writer, "20240101000000",
             "20491231235959") == 0 &&
         mbedtls_x509write_crt_set_basic_constraints(
             &contexts->certificate_writer, 0, -1) == 0 &&
         mbedtls_x509write_crt_set_key_usage(
             &contexts->certificate_writer,
             MBEDTLS_X509_KU_DIGITAL_SIGNATURE |
                 MBEDTLS_X509_KU_KEY_AGREEMENT) == 0 &&
         mbedtls_x509write_crt_set_subject_key_identifier(
             &contexts->certificate_writer) == 0 &&
         mbedtls_x509write_crt_set_authority_key_identifier(
             &contexts->certificate_writer) == 0;
  }
  ok = ok && mbedtls_x509write_crt_pem(
                 &contexts->certificate_writer, certificate_buffer,
                 kTlsCertificateBufferBytes, mbedtls_ctr_drbg_random,
                 &contexts->random) == 0;
  ok = ok && mbedtls_pk_write_key_pem(
                 &contexts->key, key_buffer,
                 kTlsPrivateKeyBufferBytes) == 0;
  ok = ok && mbedtls_x509_crt_parse(
                 &contexts->parsed, certificate_buffer,
                 strlen(reinterpret_cast<const char*>(certificate_buffer)) + 1) == 0;
  ok = ok && mbedtls_sha256(contexts->parsed.raw.p,
                            contexts->parsed.raw.len, digest, 0) == 0;
  if (ok) {
    certificate = reinterpret_cast<const char*>(certificate_buffer);
    private_key = reinterpret_cast<const char*>(key_buffer);
    fingerprint = encodeHex(digest, sizeof(digest));
  }

  mbedtls_x509_crt_free(&contexts->parsed);
  mbedtls_x509write_crt_free(&contexts->certificate_writer);
  mbedtls_pk_free(&contexts->key);
  mbedtls_ctr_drbg_free(&contexts->random);
  mbedtls_entropy_free(&contexts->entropy);
  mbedtls_platform_zeroize(contexts, sizeof(*contexts));
  delete contexts;
  mbedtls_platform_zeroize(key_buffer, kTlsPrivateKeyBufferBytes);
  heap_caps_free(key_buffer);
  heap_caps_free(certificate_buffer);
  mbedtls_platform_zeroize(serial_bytes, sizeof(serial_bytes));
  mbedtls_platform_zeroize(digest, sizeof(digest));
  return ok;
}

void WifiDeviceTransport::tlsIdentityTask(void* context) {
  auto* job = static_cast<ProvisioningJob*>(context);
  if (job && job->owner) {
    job->generated = job->owner->generateTlsIdentity(
        job->certificate, job->private_key, job->fingerprint);
    job->stack_min_free_bytes = uxTaskGetStackHighWaterMark(nullptr);
    QueueHandle_t results = job->owner->provisioning_results_;
    if (results && xQueueSend(results, &job, portMAX_DELAY) == pdTRUE) {
      vTaskDelete(nullptr);
      return;
    }
  }
  if (job) {
    zeroizeString(job->password);
    zeroizeString(job->pair_secret);
    zeroizeString(job->private_key);
    delete job;
  }
  vTaskDelete(nullptr);
}

bool WifiDeviceTransport::startTlsProvisioning(
    const TransportContext& target, const String& request_id,
    const String& ssid, const String& password, const String& pair_secret) {
  if (!provisioning_results_ || provisioning_active_) return false;
  auto* job = new (std::nothrow) ProvisioningJob;
  if (!job) return false;
  job->owner = this;
  job->target = target;
  job->request_id = request_id;
  job->ssid = ssid;
  job->password = password;
  job->pair_secret = pair_secret;
  if (job->request_id != request_id || job->ssid != ssid ||
      job->password != password || job->pair_secret != pair_secret) {
    zeroizeString(job->password);
    zeroizeString(job->pair_secret);
    delete job;
    return false;
  }
  provisioning_active_ = true;
  const BaseType_t created = xTaskCreatePinnedToCore(
      &WifiDeviceTransport::tlsIdentityTask, "s3-tls-id",
      kTlsIdentityTaskStackBytes, job, tskIDLE_PRIORITY, nullptr,
      ARDUINO_RUNNING_CORE);
  if (created == pdPASS) return true;
  provisioning_active_ = false;
  zeroizeString(job->password);
  zeroizeString(job->pair_secret);
  delete job;
  return false;
}

void WifiDeviceTransport::pollTlsProvisioning() {
  if (!provisioning_results_) return;
  ProvisioningJob* job = nullptr;
  if (xQueueReceive(provisioning_results_, &job, 0) != pdTRUE) return;
  provisioning_active_ = false;
  if (!job) return;
  tls_identity_stack_min_free_bytes_ = job->stack_min_free_bytes;

  const bool admitted = transport_ && transport_->admitted(job->target);
  if (admitted && job->generated && !job->certificate.isEmpty() &&
      !job->private_key.isEmpty() && validHex(job->fingerprint, 64)) {
    zeroizeString(tls_key_);
    tls_cert_ = std::move(job->certificate);
    tls_key_ = std::move(job->private_key);
    tls_fingerprint_ = std::move(job->fingerprint);
    if (persistConfiguration(job->ssid, job->password, job->pair_secret)) {
      sendConfigAck(job->target, job->request_id.c_str(), true, "configured",
                    "Wi-Fi configuration written");
      startConnection(true);
    } else {
      sendConfigAck(job->target, job->request_id.c_str(), false,
                    "storage_failed",
                    "Wi-Fi configuration persistence failed");
    }
  } else if (admitted) {
    sendConfigAck(job->target, job->request_id.c_str(), false,
                  "tls_identity_failed",
                  "device TLS identity generation failed");
  }

  zeroizeString(job->password);
  zeroizeString(job->pair_secret);
  zeroizeString(job->private_key);
  delete job;
}

void WifiDeviceTransport::startConnection(bool immediate) {
  if (!configured_) return;
  if (!immediate && !elapsed(millis(), next_connect_at_)) return;
  stopServer();
  stopMdns();
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  // 实时语音期间避免 modem-sleep 造成几十到数百毫秒的突发抖动。
  WiFi.setSleep(false);
  String hostname = board_device_id_;
  hostname.replace("_", "-");
  WiFi.setHostname(hostname.c_str());
  WiFi.begin(ssid_.c_str(), password_.c_str());
  connecting_ = true;
  connect_started_at_ = millis();
}

void WifiDeviceTransport::startMdns() {
  if (mdns_running_ || WiFi.status() != WL_CONNECTED) return;
  String hostname = board_device_id_;
  hostname.replace("_", "-");
  if (!MDNS.begin(hostname.c_str())) return;
  MDNS.addService(kServiceType, kServiceProtocol, kSecurePort);
  MDNS.addServiceTxt(kServiceType, kServiceProtocol, "id", board_device_id_);
  MDNS.addServiceTxt(kServiceType, kServiceProtocol, "runtime", "esp32-s3-tkeyboard");
  MDNS.addServiceTxt(kServiceType, kServiceProtocol, "schema", "8");
  MDNS.addServiceTxt(kServiceType, kServiceProtocol, "tls", "1");
  mdns_running_ = true;
}

void WifiDeviceTransport::stopMdns() {
  if (!mdns_running_) return;
  MDNS.end();
  mdns_running_ = false;
}

bool WifiDeviceTransport::startServer() {
  if (server_ || !tlsReady() || WiFi.status() != WL_CONNECTED) return server_ != nullptr;
  httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
  config.httpd.max_open_sockets = 4;
  config.httpd.max_uri_handlers = 2;
  config.httpd.stack_size = 12288;
  // 1 秒 SO_SNDTIMEO 会让一次 Wi-Fi 重传直接判定发送失败并关闭会话，
  // 表现为播放后断线重连/代次变化。给 TLS 发送足够余量，真正的死连接
  // 仍由会话看门狗（lease）和 TCP 自身处理。
  config.httpd.recv_wait_timeout = 5;
  config.httpd.send_wait_timeout = 10;
  config.servercert = reinterpret_cast<const uint8_t*>(tls_cert_.c_str());
  config.servercert_len = tls_cert_.length() + 1;
  config.prvtkey_pem = reinterpret_cast<const uint8_t*>(tls_key_.c_str());
  config.prvtkey_len = tls_key_.length() + 1;
  config.port_secure = kSecurePort;
  config.tls_handshake_timeout_ms = 5000;
  if (httpd_ssl_start(&server_, &config) != ESP_OK) {
    server_ = nullptr;
    return false;
  }

  httpd_uri_t websocket{};
  websocket.uri = kWebsocketPath;
  websocket.method = HTTP_GET;
  websocket.handler = &WifiDeviceTransport::websocketHandler;
  websocket.user_ctx = this;
  websocket.is_websocket = true;
  websocket.handle_ws_control_frames = true;
  websocket.supported_subprotocol = kWebsocketSubprotocol;
  if (httpd_register_uri_handler(server_, &websocket) != ESP_OK) {
    httpd_ssl_stop(server_);
    server_ = nullptr;
    return false;
  }
  startMdns();
  return true;
}

void WifiDeviceTransport::stopServer() {
  if (server_) {
    httpd_ssl_stop(server_);
    server_ = nullptr;
  }
  processSessionLifecycle();
}

void WifiDeviceTransport::pollConnection() {
  const uint32_t now = millis();
  if (!configured_) return;
  if (WiFi.status() == WL_CONNECTED) {
    connecting_ = false;
    reconnect_delay_ms_ = 1000;
    if (!server_) startServer();
    return;
  }
  if (server_) stopServer();
  stopMdns();
  if (connecting_ && now - connect_started_at_ >= kConnectTimeoutMs) {
    WiFi.disconnect(false, false);
    connecting_ = false;
    next_connect_at_ = now + reconnect_delay_ms_;
    reconnect_delay_ms_ = std::min(kMaxReconnectDelayMs, reconnect_delay_ms_ * 2);
  }
  if (!connecting_ && elapsed(now, next_connect_at_)) startConnection(true);
}

const char* WifiDeviceTransport::stateName() const {
  if (provisioning_active_) return "provisioning";
  if (!configured_) return "unconfigured";
  if (WiFi.status() == WL_CONNECTED && server_) return "online";
  if (WiFi.status() == WL_CONNECTED) return "tls-starting";
  if (connecting_) return "connecting";
  return "disconnected";
}

void WifiDeviceTransport::sendStatus(const TransportContext& target,
                                     const char* request_id) {
  JsonDocument payload;
  payload["requestId"] = request_id ? request_id : "";
  payload["boardDeviceId"] = board_device_id_;
  payload["configured"] = configured_;
  payload["connected"] = connected();
  payload["serverRunning"] = serverRunning();
  bool authenticated = false;
  bool client_connected = false;
  bool challenge_pending = false;
  if (session_mutex_) {
    xSemaphoreTake(session_mutex_, portMAX_DELAY);
    client_connected = session_.fd >= 0 && !session_.closed;
    authenticated = client_connected && session_.authenticated;
    challenge_pending = client_connected && session_.challenge_pending;
    xSemaphoreGive(session_mutex_);
  }
  payload["clientConnected"] = client_connected;
  payload["challengePending"] = challenge_pending;
  payload["authenticated"] = authenticated;
  payload["state"] = stateName();
  payload["provisioning"] = provisioning_active_;
  if (tls_identity_stack_min_free_bytes_ != 0) {
    payload["tlsIdentityStackMinFreeBytes"] =
        tls_identity_stack_min_free_bytes_;
  }
  payload["transport"] = "wifi-websocket";
  payload["secure"] = true;
  payload["port"] = kSecurePort;
  payload["service"] = String("_") + kServiceType + "._" + kServiceProtocol;
  if (!tls_fingerprint_.isEmpty()) payload["tlsFingerprintSha256"] = tls_fingerprint_;
  if (connected()) {
    payload["ip"] = WiFi.localIP().toString();
    payload["rssi"] = WiFi.RSSI();
    payload["ssid"] = WiFi.SSID();
  }
  transport_->sendTo(target, "network/status", payload.as<JsonObjectConst>());
}

void WifiDeviceTransport::sendConfigAck(const TransportContext& target,
                                        const char* request_id, bool ok,
                                        const char* code, const char* message) {
  JsonDocument payload;
  payload["requestId"] = request_id ? request_id : "";
  payload["boardDeviceId"] = board_device_id_;
  payload["ok"] = ok;
  payload["code"] = code;
  payload["message"] = message;
  payload["configured"] = configured_;
  payload["secure"] = true;
  if (!tls_fingerprint_.isEmpty()) payload["tlsFingerprintSha256"] = tls_fingerprint_;
  transport_->sendTo(target, "network/config-ack", payload.as<JsonObjectConst>());
}

void WifiDeviceTransport::configure(JsonObjectConst payload,
                                    const TransportContext& source) {
  const char* request_id = payload["requestId"] | "";
  if (source.kind != TransportKind::Usb || !transport_->admitted(source)) {
    sendConfigAck(source, request_id, false, "usb_required",
                  "Wi-Fi configuration requires the authenticated USB transport");
    return;
  }
  if (provisioning_active_) {
    sendConfigAck(source, request_id, false, "provisioning_busy",
                  "TLS identity generation is already in progress");
    return;
  }

  const String ssid = payload["ssid"] | "";
  String password = payload["password"] | "";
  String pair_secret = payload["pairSecret"] | "";
  const bool raw_psk = password.length() == 64 && validHex(password, 64);
  if (ssid.isEmpty() || ssid.length() > 32 ||
      (password.length() > 63 && !raw_psk) || !validHex(pair_secret, 64)) {
    zeroizeString(password);
    zeroizeString(pair_secret);
    sendConfigAck(source, request_id, false, "invalid_config",
                  "ssid, password, or 32-byte pair secret is invalid");
    return;
  }

  if (!tls_cert_.isEmpty() && !tls_key_.isEmpty() &&
      validHex(tls_fingerprint_, 64)) {
    const bool persisted = persistConfiguration(ssid, password, pair_secret);
    zeroizeString(password);
    zeroizeString(pair_secret);
    if (!persisted) {
      sendConfigAck(source, request_id, false, "storage_failed",
                    "Wi-Fi configuration persistence failed");
      return;
    }
    sendConfigAck(source, request_id, true, "configured",
                  "Wi-Fi configuration written");
    startConnection(true);
    return;
  }

  zeroizeString(tls_key_);
  tls_cert_ = "";
  tls_fingerprint_ = "";
  const bool started = startTlsProvisioning(
      source, request_id, ssid, password, pair_secret);
  zeroizeString(password);
  zeroizeString(pair_secret);
  if (!started) {
    sendConfigAck(source, request_id, false, "tls_identity_unavailable",
                  "TLS identity task could not be started");
  }
}

void WifiDeviceTransport::forget(JsonObjectConst payload,
                                 const TransportContext& source) {
  const char* request_id = payload["requestId"] | "";
  if (source.kind != TransportKind::Usb || !transport_->admitted(source)) {
    sendConfigAck(source, request_id, false, "usb_required",
                  "forget requires the authenticated USB transport");
    return;
  }
  if (provisioning_active_) {
    sendConfigAck(source, request_id, false, "provisioning_busy",
                  "TLS identity generation is already in progress");
    return;
  }
  clearConfiguration();
  sendConfigAck(source, request_id, true, "forgotten",
                "Wi-Fi credentials, pairing secret, and TLS identity erased");
}

void WifiDeviceTransport::beginScan(JsonObjectConst payload,
                                    const TransportContext& source) {
  const char* request_id = payload["requestId"] | "";
  if (source.kind != TransportKind::Usb || !transport_->admitted(source)) {
    JsonDocument response;
    response["requestId"] = request_id;
    response["ok"] = false;
    response["code"] = "usb_required";
    transport_->sendTo(source, "network/scan-result", response.as<JsonObjectConst>());
    return;
  }
  if (scan_active_) {
    JsonDocument response;
    response["requestId"] = request_id;
    response["ok"] = false;
    response["code"] = "scan_busy";
    transport_->sendTo(source, "network/scan-result", response.as<JsonObjectConst>());
    return;
  }
  WiFi.mode(WIFI_STA);
  const int result = WiFi.scanNetworks(true, false, false, 300);
  if (result == WIFI_SCAN_FAILED) {
    JsonDocument response;
    response["requestId"] = request_id;
    response["ok"] = false;
    response["code"] = "scan_failed";
    transport_->sendTo(source, "network/scan-result", response.as<JsonObjectConst>());
    return;
  }
  scan_active_ = true;
  scan_request_id_ = request_id;
  scan_target_ = source;
  JsonDocument response;
  response["requestId"] = request_id;
  response["ok"] = true;
  response["state"] = "scanning";
  transport_->sendTo(source, "network/scan-state", response.as<JsonObjectConst>());
}

void WifiDeviceTransport::pollScan() {
  if (!scan_active_) return;
  const int result = WiFi.scanComplete();
  if (result == WIFI_SCAN_RUNNING) return;
  JsonDocument response;
  response["requestId"] = scan_request_id_;
  response["ok"] = result >= 0;
  response["state"] = result >= 0 ? "complete" : "failed";
  JsonArray networks = response["networks"].to<JsonArray>();
  if (result >= 0) {
    const int count = std::min<int>(result, kMaxNetworks);
    for (int index = 0; index < count; ++index) {
      JsonObject network = networks.add<JsonObject>();
      network["ssid"] = WiFi.SSID(index);
      network["rssi"] = WiFi.RSSI(index);
      network["channel"] = WiFi.channel(index);
      network["auth"] = authModeName(WiFi.encryptionType(index));
    }
  } else {
    response["code"] = "scan_failed";
  }
  transport_->sendTo(scan_target_, "network/scan-result", response.as<JsonObjectConst>());
  WiFi.scanDelete();
  scan_active_ = false;
  scan_request_id_ = "";
  scan_target_ = {};
}

bool WifiDeviceTransport::verifyProof(const String& client_id,
                                      const String& nonce,
                                      const String& proof) const {
  if (!validHex(pair_secret_, 64) || !validHex(proof, 64)) return false;
  uint8_t secret[32] = {};
  uint8_t expected[32] = {};
  uint8_t supplied[32] = {};
  if (!decodeHex(pair_secret_, secret, sizeof(secret)) ||
      !decodeHex(proof, supplied, sizeof(supplied))) return false;
  const String message = String("hachimodock-wifi-v1\n") + board_device_id_ + "\n" +
                         boot_id_ + "\n" + client_id + "\n" + nonce;
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  const bool calculated = info && mbedtls_md_hmac(
      info, secret, sizeof(secret),
      reinterpret_cast<const unsigned char*>(message.c_str()), message.length(), expected) == 0;
  const bool matches = calculated &&
      s3transport::constantTimeEqual(expected, supplied, sizeof(expected));
  memset(secret, 0, sizeof(secret));
  memset(expected, 0, sizeof(expected));
  memset(supplied, 0, sizeof(supplied));
  return matches;
}

void WifiDeviceTransport::sendAuthAck(const TransportContext& target,
                                      const char* request_id, bool ok,
                                      const char* code) {
  JsonDocument payload;
  payload["requestId"] = request_id ? request_id : "";
  payload["boardDeviceId"] = board_device_id_;
  payload["bootId"] = boot_id_;
  payload["ok"] = ok;
  payload["code"] = code;
  payload["secure"] = true;
  payload["connectionGeneration"] = target.generation;
  transport_->sendTo(target, "network/auth-ack", payload.as<JsonObjectConst>());
}

bool WifiDeviceTransport::authenticate(JsonObjectConst payload,
                                       const TransportContext& source,
                                       bool& authenticated_now) {
  authenticated_now = false;
  if (source.kind != TransportKind::Wifi || source.peer_id == 0) return true;
  const String request_id = payload["requestId"] | "";
  const String client_id = payload["clientId"] | "";
  const String nonce = payload["nonce"] | "";
  const String proof = payload["proof"] | "";

  xSemaphoreTake(session_mutex_, portMAX_DELAY);
  const bool matching_session = session_.fd == static_cast<int>(source.peer_id) &&
                                !session_.closed && !session_.authenticated &&
                                !session_.nonce.isEmpty();
  const bool challenge_valid = matching_session &&
                               !elapsed(millis(), session_.challenge_deadline) &&
                               nonce == session_.nonce && !client_id.isEmpty() &&
                               client_id.length() <= kMaxClientIdLength;
  xSemaphoreGive(session_mutex_);

  if (!challenge_valid || !verifyProof(client_id, nonce, proof)) {
    xSemaphoreTake(session_mutex_, portMAX_DELAY);
    const bool still_matching =
        session_.fd == static_cast<int>(source.peer_id) && !session_.closed &&
        !session_.authenticated && !session_.nonce.isEmpty();
    if (still_matching && ++session_.failures >= 3) session_.closed = true;
    const bool close = still_matching && session_.closed;
    xSemaphoreGive(session_mutex_);
    sendAuthAck(source, request_id.c_str(), false, "authentication_failed");
    if (close) closePeer(source.peer_id);
    return true;
  }

  xSemaphoreTake(session_mutex_, portMAX_DELAY);
  const bool reserved =
      session_.fd == static_cast<int>(source.peer_id) && !session_.closed &&
      !session_.authenticated && !session_.nonce.isEmpty() &&
      nonce == session_.nonce;
  if (reserved) session_.nonce = "";
  xSemaphoreGive(session_mutex_);
  if (!reserved) {
    sendAuthAck(source, request_id.c_str(), false, "authentication_superseded");
    return true;
  }

  TransportContext candidate = source;
  candidate.authenticated = true;
  candidate.valid = true;
  const TransportContext previous_owner = transport_->owner();
  const TransportContext claimed = transport_->claimOwner(candidate);
  xSemaphoreTake(session_mutex_, portMAX_DELAY);
  const bool committed = session_.fd == static_cast<int>(source.peer_id) &&
                         !session_.closed && !session_.authenticated;
  if (committed) {
    session_.authenticated = true;
    session_.generation = claimed.generation;
    session_.nonce = "";
    session_.last_rx = millis();
  }
  xSemaphoreGive(session_mutex_);
  if (!committed) {
    transport_->releaseOwner(claimed);
    if (previous_owner.kind == TransportKind::Usb &&
        previous_owner.valid && previous_owner.authenticated)
      transport_->claimOwner(previous_owner, true);
    sendAuthAck(source, request_id.c_str(), false, "authentication_superseded");
    closePeer(source.peer_id);
    return true;
  }
  sendAuthAck(claimed, request_id.c_str(), true, "authenticated");
  authenticated_now = true;
  return true;
}

bool WifiDeviceTransport::handleControl(const char* topic, JsonObjectConst payload,
                                        const TransportContext& source,
                                        bool& authenticated_now) {
  authenticated_now = false;
  if (!transport_ || !session_mutex_) return false;
  if (!strcmp(topic, "network/auth"))
    return authenticate(payload, source, authenticated_now);
  if (!strcmp(topic, "network/challenge-request")) {
    if (source.kind != TransportKind::Wifi) return true;
    xSemaphoreTake(session_mutex_, portMAX_DELAY);
    const bool matching_session =
        session_.fd == static_cast<int>(source.peer_id) &&
        session_.epoch == source.generation && !session_.closed &&
        !session_.authenticated;
    xSemaphoreGive(session_mutex_);
    if (!matching_session) closePeer(source.peer_id);
    return true;
  }
  if (!strcmp(topic, "network/config")) {
    configure(payload, source);
    return true;
  }
  if (!strcmp(topic, "network/forget")) {
    forget(payload, source);
    return true;
  }
  if (!strcmp(topic, "network/scan")) {
    beginScan(payload, source);
    return true;
  }
  if (!strcmp(topic, "network/status-query")) {
    if (source.kind == TransportKind::Usb || transport_->admitted(source))
      sendStatus(source, payload["requestId"] | "");
    return true;
  }
  return false;
}

TransportContext WifiDeviceTransport::sessionContext(bool authenticated_override,
                                                       bool allow_closed) const {
  TransportContext context;
  context.kind = TransportKind::Wifi;
  context.peer_id = session_.fd < 0 ? 0 : static_cast<uint32_t>(session_.fd);
  context.generation = session_.authenticated ? session_.generation : session_.epoch;
  context.authenticated = authenticated_override || session_.authenticated;
  context.valid = session_.fd >= 0 && (allow_closed || !session_.closed);
  return context;
}

void WifiDeviceTransport::sendChallenge() {
  TransportContext target;
  String nonce;
  uint32_t epoch = 0;
  xSemaphoreTake(session_mutex_, portMAX_DELAY);
  if (!session_.challenge_pending || session_.closed || session_.fd < 0) {
    xSemaphoreGive(session_mutex_);
    return;
  }
  target = sessionContext(false);
  nonce = session_.nonce;
  epoch = session_.epoch;
  xSemaphoreGive(session_mutex_);

  JsonDocument envelope;
  envelope["topic"] = "network/challenge";
  JsonObject payload = envelope["payload"].to<JsonObject>();
  payload["boardDeviceId"] = board_device_id_;
  payload["bootId"] = boot_id_;
  payload["nonce"] = nonce;
  payload["algorithm"] = "hmac-sha256";
  payload["expiresMs"] = kChallengeLifetimeMs;
  payload["secure"] = true;
  String message;
  message.reserve(measureJson(envelope) + 1);
  if (serializeJson(envelope, message) > 0 &&
      sendText(target, message) == TransportSendResult::Sent) {
    xSemaphoreTake(session_mutex_, portMAX_DELAY);
    if (session_.fd == static_cast<int>(target.peer_id) &&
        session_.epoch == epoch && session_.nonce == nonce)
      session_.challenge_pending = false;
    xSemaphoreGive(session_mutex_);
  }
}

bool WifiDeviceTransport::enqueueFrame(int fd, uint32_t epoch,
                                       const String& text) {
  if (!session_mutex_) return false;
  xSemaphoreTake(session_mutex_, portMAX_DELAY);
  if (pending_count_ >= sizeof(pending_) / sizeof(pending_[0])) {
    xSemaphoreGive(session_mutex_);
    return false;
  }
  const size_t tail = (pending_head_ + pending_count_) %
                      (sizeof(pending_) / sizeof(pending_[0]));
  pending_[tail].fd = fd;
  pending_[tail].epoch = epoch;
  pending_[tail].text = text;
  ++pending_count_;
  xSemaphoreGive(session_mutex_);
  return true;
}

bool WifiDeviceTransport::takeFrame(PendingFrame& frame) {
  xSemaphoreTake(session_mutex_, portMAX_DELAY);
  if (pending_count_ == 0) {
    xSemaphoreGive(session_mutex_);
    return false;
  }
  frame.fd = pending_[pending_head_].fd;
  frame.epoch = pending_[pending_head_].epoch;
  frame.text = std::move(pending_[pending_head_].text);
  pending_[pending_head_].fd = -1;
  pending_[pending_head_].epoch = 0;
  pending_[pending_head_].text = "";
  pending_head_ = (pending_head_ + 1) % (sizeof(pending_) / sizeof(pending_[0]));
  --pending_count_;
  xSemaphoreGive(session_mutex_);
  return true;
}

void WifiDeviceTransport::processPendingFrames() {
  PendingFrame frame;
  while (transport_->inboundAvailable() && takeFrame(frame)) {
    xSemaphoreTake(session_mutex_, portMAX_DELAY);
    const TransportContext context =
        session_.fd == frame.fd && session_.epoch == frame.epoch
            ? sessionContext(false) : TransportContext{};
    xSemaphoreGive(session_mutex_);
    if (context.valid && !transport_->enqueueInbound(context, frame.text)) {
      markSessionClosed(frame.fd, frame.epoch);
      return;
    }
  }
}

void WifiDeviceTransport::markSessionClosed(int fd, uint32_t epoch) {
  if (!session_mutex_) return;
  xSemaphoreTake(session_mutex_, portMAX_DELAY);
  if (session_.fd == fd && session_.epoch == epoch)
    session_.closed = true;
  xSemaphoreGive(session_mutex_);
}

void WifiDeviceTransport::completeSend(int fd, uint32_t epoch, esp_err_t error) {
  if (!session_mutex_) return;
  xSemaphoreTake(session_mutex_, portMAX_DELAY);
  if (session_.fd == fd && session_.epoch == epoch) {
    if (session_.sends_inflight > 0) --session_.sends_inflight;
    // 异步发送回调返回错误时，当前 socket 的发送结果已经未知；不能继续
    // 把它当成健康连接，否则 ACK 会永远等不到、旧 fd 还会挡住下一次
    // challenge。统一进入 session 退役流程，由 PC 的 reconnect profile 重建。
    if (error != ESP_OK) session_.closed = true;
  }
  xSemaphoreGive(session_mutex_);
}

void WifiDeviceTransport::processSessionLifecycle() {
  TransportContext released;
  bool should_release = false;
  bool close_socket = false;
  xSemaphoreTake(session_mutex_, portMAX_DELAY);
  if (server_ && session_.fd >= 0 && !session_.closed &&
      httpd_ws_get_fd_info(server_, session_.fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
    // sessionFree 可能尚未执行，但 fd 已经不是活动 WebSocket；立即进入
    // 统一释放路径，不能让死连接占住 challenge/owner。
    session_.closed = true;
  }
  if (session_.closed && session_.fd >= 0) {
    released = sessionContext(false, true);
    should_release = session_.authenticated;
    close_socket = true;
    session_.fd = -1;
    session_.epoch = 0;
    session_.generation = 0;
    session_.authenticated = false;
    session_.challenge_pending = false;
    session_.connected_at = 0;
    session_.challenge_deadline = 0;
    session_.last_rx = 0;
    session_.nonce = "";
    session_.fragments = "";
    session_.failures = 0;
    session_.sends_inflight = 0;
  } else if (!session_.authenticated && session_.fd >= 0 &&
             elapsed(millis(), session_.challenge_deadline)) {
    released = sessionContext(false);
    session_.closed = true;
    close_socket = true;
  } else if (session_.authenticated &&
             millis() - session_.last_rx > kAuthenticatedLeaseMs) {
    released = sessionContext(false);
    session_.closed = true;
    should_release = true;
    close_socket = true;
  }
  xSemaphoreGive(session_mutex_);
  if (should_release) transport_->releaseOwner(released);
  if (close_socket) closePeer(released.peer_id);
}

void WifiDeviceTransport::poll() {
  if (!transport_ || !session_mutex_ || !provisioning_results_) return;
  pollTlsProvisioning();
  pollConnection();
  pollScan();
  processPendingFrames();
  sendChallenge();
  processSessionLifecycle();
}

TransportSendResult WifiDeviceTransport::sendText(
    const TransportContext& target, const String& message) {
  if (!transport_ || !session_mutex_)
    return TransportSendResult::Failed;
  if (!server_ || target.kind != TransportKind::Wifi || target.peer_id == 0)
    return TransportSendResult::Failed;
  int fd = -1;
  uint32_t epoch = 0;
  bool busy = false;
  xSemaphoreTake(session_mutex_, portMAX_DELAY);
  const bool preauth_target = !target.authenticated &&
                              target.generation == session_.epoch;
  const bool owner_target = target.authenticated && session_.authenticated &&
                            target.generation == session_.generation;
  if (session_.fd == static_cast<int>(target.peer_id) && !session_.closed &&
      (preauth_target || owner_target)) {
    if (session_.sends_inflight < 8) {
      fd = session_.fd;
      epoch = session_.epoch;
      ++session_.sends_inflight;
    } else {
      busy = true;
    }
  }
  xSemaphoreGive(session_mutex_);
  if (busy) return TransportSendResult::Busy;
  if (fd < 0) return TransportSendResult::Failed;
  if (httpd_ws_get_fd_info(server_, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
    completeSend(fd, epoch, ESP_FAIL);
    return TransportSendResult::Failed;
  }

  auto* outbound = new (std::nothrow) OutboundFrame;
  if (!outbound) {
    completeSend(fd, epoch, ESP_OK);
    return TransportSendResult::Busy;
  }
  outbound->owner = this;
  outbound->fd = fd;
  outbound->epoch = epoch;
  outbound->text = message;
  outbound->frame.final = true;
  outbound->frame.fragmented = false;
  outbound->frame.type = HTTPD_WS_TYPE_TEXT;
  outbound->frame.payload = reinterpret_cast<uint8_t*>(
      const_cast<char*>(outbound->text.c_str()));
  outbound->frame.len = outbound->text.length();
  const esp_err_t result = httpd_ws_send_data_async(
      server_, fd, &outbound->frame, &WifiDeviceTransport::sendComplete, outbound);
  if (result != ESP_OK) {
    completeSend(fd, epoch, result == ESP_ERR_NO_MEM ? ESP_OK : result);
    delete outbound;
    return result == ESP_ERR_NO_MEM ? TransportSendResult::Busy
                                    : TransportSendResult::Failed;
  }
  return TransportSendResult::Sent;
}

void WifiDeviceTransport::closePeer(uint32_t peer_id) {
  if (server_ && peer_id != 0)
    httpd_sess_trigger_close(server_, static_cast<int>(peer_id));
}

void WifiDeviceTransport::sessionFree(void* context) {
  auto* session = static_cast<SessionContext*>(context);
  if (!session) return;
  if (session->owner)
    session->owner->markSessionClosed(session->fd, session->epoch);
  delete session;
}

void WifiDeviceTransport::sendComplete(esp_err_t error, int fd, void* context) {
  auto* outbound = static_cast<OutboundFrame*>(context);
  if (!outbound) return;
  if (outbound->owner)
    outbound->owner->completeSend(fd, outbound->epoch, error);
  delete outbound;
}

esp_err_t WifiDeviceTransport::websocketHandler(httpd_req_t* request) {
  if (!request || !request->user_ctx) return ESP_ERR_INVALID_ARG;
  return static_cast<WifiDeviceTransport*>(request->user_ctx)->handleWebsocket(request);
}

esp_err_t WifiDeviceTransport::handleWebsocket(httpd_req_t* request) {
  const int fd = httpd_req_to_sockfd(request);
  auto* context = static_cast<SessionContext*>(request->sess_ctx);
  if (!context) {
    context = new (std::nothrow) SessionContext{this, fd, 0};
    if (!context) return ESP_ERR_NO_MEM;
    xSemaphoreTake(session_mutex_, portMAX_DELAY);
  if (session_.fd >= 0 && !session_.closed && session_.fd != fd) {
    // HTTPD 的 sessionFree 可能晚于 TCP 实际断开到达。小智的 WebSocket
    // 接收任务会在断线回调中立即清理；这里也主动检查旧 fd，避免死 fd
    // 把下一次 challenge 永久挡在单客户端准入之前。
    const bool old_socket_alive = server_ &&
        httpd_ws_get_fd_info(server_, session_.fd) == HTTPD_WS_CLIENT_WEBSOCKET;
    if (!old_socket_alive) {
      session_.fd = -1;
      session_.epoch = 0;
      session_.generation = 0;
      session_.authenticated = false;
      session_.challenge_pending = false;
      session_.closed = false;
      session_.nonce = "";
      session_.fragments = "";
      session_.sends_inflight = 0;
    } else {
      xSemaphoreGive(session_mutex_);
      delete context;
      return ESP_FAIL;
    }
  }
    const uint32_t epoch = next_session_epoch_++;
    if (next_session_epoch_ == 0) next_session_epoch_ = 1;
    context->epoch = epoch;
    session_.fd = fd;
    session_.epoch = epoch;
    session_.generation = 0;
    session_.authenticated = false;
    session_.challenge_pending = true;
    session_.closed = false;
    session_.failures = 0;
    session_.sends_inflight = 0;
    const uint32_t connected_at = millis();
    session_.connected_at = connected_at;
    session_.challenge_deadline = connected_at + kChallengeLifetimeMs;
    session_.last_rx = connected_at;
    session_.nonce = randomHex(16);
    session_.fragments = "";
    request->sess_ctx = context;
    request->free_ctx = &WifiDeviceTransport::sessionFree;
    xSemaphoreGive(session_mutex_);
  }

  if (context->owner != this || context->fd != fd) return ESP_FAIL;
  xSemaphoreTake(session_mutex_, portMAX_DELAY);
  const bool active_session = session_.fd == fd &&
                              session_.epoch == context->epoch &&
                              !session_.closed;
  xSemaphoreGive(session_mutex_);
  if (!active_session) return ESP_FAIL;
  httpd_ws_frame_t frame{};
  const esp_err_t header = httpd_ws_recv_frame(request, &frame, 0);
  // 空闲（例如播放结束、等待用户说话）时的接收超时不是会话错误：
  // 返回 ESP_FAIL 会让 httpd 关闭 WebSocket，PC 重连后连接代次改变，
  // 实时对话随即报「设备连接已变化」。保持会话并等待下一帧。
  if (header == HTTPD_SOCK_ERR_TIMEOUT || header == ESP_ERR_TIMEOUT) return ESP_OK;
  if (header != ESP_OK) return ESP_FAIL;
  if (frame.len > ProtocolTransport::kMaxMessageBytes) {
    closePeer(fd);
    return ESP_ERR_INVALID_SIZE;
  }
  uint8_t* payload = nullptr;
  if (frame.len > 0) {
    payload = static_cast<uint8_t*>(malloc(frame.len + 1));
    if (!payload) return ESP_ERR_NO_MEM;
    frame.payload = payload;
    if (httpd_ws_recv_frame(request, &frame, frame.len) != ESP_OK) {
      free(payload);
      return ESP_FAIL;
    }
    payload[frame.len] = '\0';
  }

  xSemaphoreTake(session_mutex_, portMAX_DELAY);
  session_.last_rx = millis();
  xSemaphoreGive(session_mutex_);

  if (frame.type == HTTPD_WS_TYPE_PING) {
    frame.type = HTTPD_WS_TYPE_PONG;
    // 单次 PONG 发送超时同样不应升级为断线；真正的死连接由后续 recv 判定。
    (void)httpd_ws_send_frame(request, &frame);
    free(payload);
    return ESP_OK;
  }
  if (frame.type == HTTPD_WS_TYPE_PONG) {
    free(payload);
    return ESP_OK;
  }
  if (frame.type == HTTPD_WS_TYPE_CLOSE) {
    free(payload);
    markSessionClosed(fd, context->epoch);
    return ESP_OK;
  }
  if (frame.type != HTTPD_WS_TYPE_TEXT && frame.type != HTTPD_WS_TYPE_CONTINUE) {
    free(payload);
    closePeer(fd);
    return ESP_ERR_INVALID_ARG;
  }

  String complete;
  bool ready = false;
  xSemaphoreTake(session_mutex_, portMAX_DELAY);
  if (frame.type == HTTPD_WS_TYPE_TEXT && !session_.fragments.isEmpty()) {
    session_.closed = true;
  } else if (session_.fragments.length() + frame.len > ProtocolTransport::kMaxMessageBytes) {
    session_.closed = true;
  } else {
    if (frame.len > 0) session_.fragments.concat(reinterpret_cast<const char*>(payload), frame.len);
    if (frame.final) {
      complete = std::move(session_.fragments);
      session_.fragments = "";
      ready = true;
    }
  }
  const bool close = session_.closed;
  xSemaphoreGive(session_mutex_);
  free(payload);
  if (close) {
    closePeer(fd);
    return ESP_FAIL;
  }
  if (ready && !enqueueFrame(fd, context->epoch, complete)) {
    closePeer(fd);
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}
