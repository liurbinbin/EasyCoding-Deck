#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include "board_runtime.h"
#include "audio_service.h"
#include "pin_config.h"
#include "power_manager.h"
#include "protocol_transport.h"
#include "status_led_service.h"
#include "usb_cdc_transport.h"
#include "wifi_transport.h"

namespace {
constexpr char kRuntime[] = "esp32-s3-tkeyboard";
constexpr char kUsbWireProtocol[] = "pet-usb-jsonl-v4";
constexpr char kWifiWireProtocol[] = "pet-wss-json-v1";
constexpr char kFirmwareVersion[] = "0.6.4-s3-wifi";
constexpr uint32_t kProtocolSchema = 8;
constexpr uint32_t kHeartbeatMs = 2000;
BoardRuntime board;
PowerManager power;
AudioService audio;
StatusLedService status_leds;
UsbCdcTransport usb_transport;
WifiDeviceTransport wifi_transport;
String current_agent = "idle";
String current_state = "idle";
String current_detail = "ready";
String approval_request_id;
String approval_agent;
String approval_tool;
String approval_reason;
uint32_t approval_deadline_ms = 0;
bool approval_active = false;
uint32_t state_hold_until_ms = 0;
bool audio_bridge_armed = false;
bool reboot_pending = false;
String boot_id;
uint32_t max_loop_gap_ms = 0;

StatusLedMode ledModeForState(const String& state);

String boardDeviceId() {
  return String("s3-tkeyboard-") + String((uint32_t)ESP.getEfuseMac(), HEX);
}

class DispatchGuard {
 public:
  explicit DispatchGuard(const TransportContext& source) {
    protocolTransport().beginDispatch(source);
  }
  ~DispatchGuard() { protocolTransport().endDispatch(); }
};

void sendProtocolError(const TransportContext& target, const char* code,
                       const char* message, const char* request_id = "") {
  JsonDocument payload;
  payload["requestId"] = request_id ? request_id : "";
  payload["ok"] = false;
  payload["code"] = code;
  payload["message"] = message;
  protocolTransport().sendTo(target, "protocol/error", payload.as<JsonObjectConst>());
}

void sendMessage(const char* topic, JsonObjectConst payload) {
  protocolTransport().send(topic, payload);
}

void sendHello(const TransportContext& target = protocolTransport().owner()) {
  JsonDocument doc;
  doc["topic"] = "hello";
  JsonObject payload = doc["payload"].to<JsonObject>();
  payload["boardDeviceId"] = boardDeviceId();
  payload["runtime"] = kRuntime;
  payload["deviceModel"] = "T-Keyboard Voice V0.3";
  payload["fw"] = kFirmwareVersion;
  payload["protocolSchema"] = kProtocolSchema;
  payload["wireProtocol"] = target.kind == TransportKind::Wifi
      ? kWifiWireProtocol : kUsbWireProtocol;
  payload["connectionGeneration"] = target.generation;
  payload["activeTransport"] = target.kind == TransportKind::Wifi ? "wifi" : "usb";
  JsonObject caps = payload["capabilities"].to<JsonObject>();
  caps["usbOnly"] = false;
  caps["nativeUsb"] = true;
  JsonObject transport = caps["transport"].to<JsonObject>();
  transport["usb"] = true;
  transport["wifiWebSocket"] = true;
  transport["wifiWebSocketSecure"] = true;
  transport["wifiAuthentication"] = "hmac-sha256-v1";
  transport["wifiConfigured"] = wifi_transport.configured();
  transport["wifiConnected"] = wifi_transport.connected();
  transport["wifiPort"] = WifiDeviceTransport::kSecurePort;
  transport["wifiWireProtocol"] = kWifiWireProtocol;
  if (wifi_transport.tlsReady())
    transport["tlsFingerprintSha256"] = wifi_transport.tlsFingerprint();
  caps["voice"] = true;
  caps["inputs"] = true;
  caps["inputConfig"] = true;
  caps["debugBootId"] = boot_id;
  caps["diagnostics"] = "s3-diagnostics-v2";
  caps["deviceVoice"] = "usb-jsonl-pcm-v1";
  caps["wifiDeviceVoice"] = "wifi-websocket-json-pcm-v1";
  caps["audioMasterVolume"] = "p4-master-volume-v1";
  caps["audioTestPlayback"] = "usb-pcm16-test-v1";
  caps["stateAlerts"] = "s3-state-alerts-v1";
  caps["approvalInput"] = "s3-approval-v1";
  caps["realtimeConversation"] = "s3-half-duplex-v1";
  caps["s3Avatar"] = "s3-avatar-rgb565-v3";
  caps["features"]["conversationSubtitles"] = true;
  JsonObject audio_caps = caps["audio"].to<JsonObject>();
  audio_caps["available"] = audio.available();
  audio_caps["captureAvailable"] = audio.available();
  audio_caps["playbackAvailable"] = audio.playbackAvailable();
  caps["displayTopology"]["count"] = 5;
  caps["displayTopology"]["width"] = 128;
  caps["displayTopology"]["height"] = 128;
  caps["displayTopology"]["bus"] = "shared-spi";
  caps["displayTopology"]["touch"] = false;
  JsonArray roles = caps["displayTopology"]["roles"].to<JsonArray>();
  roles.add("avatar");
  roles.add("status");
  roles.add("message");
  roles.add("controls");
  roles.add("system");
  caps["input"]["keys"] = 5;
  caps["input"]["touch"] = false;
  caps["input"]["encoder"] = false;
  audio_caps["capture"] = "ES7210";
  audio_caps["captureChannels"] = 3;
  audio_caps["captureMode"] = "mic1+mic2; mic1+mic2+mic3 when AUX is detected";
  audio_caps["sampleRate"] = 16000;
  audio_caps["playback"] = "ES8389";
  caps["power"]["pmic"] = "AXP2101";
  caps["power"]["deepSleep"] = true;
  caps["power"]["powerOff"] = true;
  caps["power"]["telemetry"] = true;
  caps["power"]["key4ActionConfig"] = true;
  caps["peripherals"]["ws2812"] = 14;
  caps["peripherals"]["qwiicI2c"] = true;
  caps["peripherals"]["qwiicUart"] = true;
  protocolTransport().sendDocumentTo(target, doc);
}

void sendDiagnostics(JsonObjectConst request) {
  JsonDocument doc;
  doc["topic"] = "diagnostics/status";
  JsonObject payload = doc["payload"].to<JsonObject>();
  payload["requestId"] = request["requestId"] | "";
  payload["boardDeviceId"] = boardDeviceId();
  payload["bootCount"] = 1;
  payload["faultResetCount"] = 0;
  payload["lastResetReason"] = "software";
  payload["lastResetWasFault"] = false;
  JsonObject runtime = payload["runtime"].to<JsonObject>();
  runtime["runtime"] = kRuntime;
  runtime["firmware"] = kFirmwareVersion;
  runtime["protocolSchema"] = kProtocolSchema;
  runtime["uptimeMs"] = millis();
  runtime["runningPartition"] = "factory";
  runtime["imageState"] = "valid";
  runtime["buildId"] = String(__DATE__) + " " + __TIME__;
  runtime["bootId"] = boot_id;
  runtime["screenPage"] = "five-panel";
  runtime["agentState"] = current_state;
  runtime["touchReady"] = false;
  runtime["touchSupported"] = false;
  runtime["taskCount"] = uxTaskGetNumberOfTasks();
  runtime["inputDroppedEvents"] = 0;
  runtime["audioCaptureActive"] = audio.active();
  runtime["audioPlaybackActive"] = audio.playbackActive();
  runtime["displayReady"] = board.displayReady();
  JsonObject transport_status = runtime["transport"].to<JsonObject>();
  const TransportContext owner = protocolTransport().owner();
  transport_status["owner"] = owner.valid
      ? (owner.kind == TransportKind::Wifi ? "wifi" : "usb") : "none";
  transport_status["generation"] = owner.generation;
  transport_status["wifiState"] = wifi_transport.stateName();
  transport_status["droppedInbound"] = protocolTransport().droppedInbound();
  transport_status["droppedOutbound"] = protocolTransport().droppedOutbound();
  transport_status["inboundHighWater"] = protocolTransport().inboundHighWater();
  transport_status["outboundHighWater"] = protocolTransport().outboundHighWater();
  transport_status["maxLoopGapMs"] = max_loop_gap_ms;
  JsonObject memory = payload["memory"].to<JsonObject>();
  memory["freeHeapBytes"] = ESP.getFreeHeap();
  memory["minimumFreeHeapBytes"] = esp_get_minimum_free_heap_size();
  memory["freePsramBytes"] = ESP.getFreePsram();
  memory["minimumFreePsramBytes"] = ESP.getMinFreePsram();
  JsonObject storage = payload["storage"].to<JsonObject>();
  storage["usedBytes"] = 0;
  storage["totalBytes"] = 0;
  storage["activeAppearanceSlot"] = nullptr;
  JsonObject power_status = payload["power"].to<JsonObject>();
  power_status["pmicAvailable"] = power.pmicAvailable();
  power_status["railsAvailable"] = power.railsAvailable();
  power_status["batteryPresent"] = power.batteryPresent();
  power_status["vbusPresent"] = power.vbusPresent();
  power_status["charging"] = power.charging();
  if (power.batteryPercent() >= 0) power_status["batteryPercent"] = power.batteryPercent();
  if (power.batteryMillivolts() >= 0) power_status["batteryMillivolts"] = power.batteryMillivolts();
  sendMessage("diagnostics/status", payload);
}

void sendActionResult(JsonObjectConst request, const char* action, const char* message) {
  JsonDocument payload;
  payload["requestId"] = request["requestId"] | "";
  payload["boardDeviceId"] = boardDeviceId();
  payload["ok"] = true;
  payload["action"] = action;
  payload["message"] = message;
  sendMessage("diagnostics/action", payload.as<JsonObjectConst>());
}

void clearApproval() {
  approval_active = false;
  approval_request_id = "";
  approval_agent = "";
  approval_tool = "";
  approval_reason = "";
  approval_deadline_ms = 0;
}

void onTransportOwnerChanged(const TransportContext& previous,
                             const TransportContext& current) {
  (void)current;
  if (!previous.valid) return;
  clearApproval();
  audio_bridge_armed = false;
  board.setVoiceCaptureActive(false);
  board.abortAvatarTransfer();
  audio.abortForTransportChange();
  status_leds.setMode(ledModeForState(current_state));
  board.renderStatus(current_agent, current_state, "transport changed");
}

void sendApprovalDecision(bool allow) {
  JsonDocument payload;
  payload["requestId"] = approval_request_id;
  payload["decision"] = allow ? "allow" : "deny";
  payload["source"] = "esp32-s3";
  sendMessage("approval/decision", payload.as<JsonObjectConst>());
  clearApproval();
  audio.notifyEvent(allow ? "permission_approved" : "permission_denied");
  status_leds.showTemporary(allow ? StatusLedMode::Success : StatusLedMode::Error,
                            1500, ledModeForState(current_state));
  board.renderStatus(current_agent, current_state, current_detail);
}

bool handleInputAction(const char* event, const char* action, const char* gesture) {
  if (approval_active) {
    if (!strcmp(gesture, "short_press")) {
      const int decision = board.approvalDecisionForShortEvent(event);
      if (decision > 0) sendApprovalDecision(true);
      else if (decision < 0) sendApprovalDecision(false);
    }
    return true;
  }
  if (!strcmp(action, "component_center") &&
      (!strcmp(gesture, "short_press") || !strcmp(gesture, "long_press"))) {
    board.selectNextAvatarState();
    return true;
  }
  if (strcmp(action, "voice_ptt") != 0) return false;
  if (!audio_bridge_armed) {
    if (!strcmp(gesture, "hold_start"))
      audio.rejectCapture("bridge-not-armed", "PC voice bridge is not ready");
    return false;
  }
  if (!strcmp(gesture, "hold_start")) {
    if (audio.startCapture()) {
      board.setVoiceCaptureActive(true);
      status_leds.setMode(StatusLedMode::Recording);
    }
  } else if (!strcmp(gesture, "hold_end")) {
    board.setVoiceCaptureActive(false);
    audio.stopCapture("button-release");
    board.renderSendResult(true, "VOICE SENT");
    status_leds.showTemporary(StatusLedMode::Success, 1500,
                              ledModeForState(current_state));
  }
  return false;
}

String normalizedState(const char* raw) {
  String state = raw ? raw : "idle";
  state.toLowerCase();
  state.replace('-', '_');
  if (state == "thinking" || state == "tool_running" || state == "working") return "WORKING";
  if (state == "speaking") return "SPEAKING";
  if (state == "waiting" || state == "waiting_user" || state == "needs_user") return "WAITING";
  if (state == "done" || state == "complete" || state == "completed") return "DONE";
  if (state == "error" || state == "failed") return "ERROR";
  return "IDLE";
}

StatusLedMode ledModeForState(const String& state) {
  if (state == "WORKING") return StatusLedMode::Working;
  if (state == "SPEAKING") return StatusLedMode::Speaking;
  if (state == "WAITING") return StatusLedMode::Waiting;
  return StatusLedMode::Idle;
}

void beginApproval(JsonObjectConst payload) {
  const String request_id = payload["requestId"] | "";
  if (request_id.isEmpty() || request_id.length() > 63) return;
  approval_request_id = request_id;
  approval_agent = payload["agent"] | "AGENT";
  approval_tool = payload["tool"] | "ACTION";
  approval_reason = payload["reason"] | "ALLOW?";
  const bool never = payload["neverExpires"] | false;
  uint32_t timeout = payload["timeoutMs"] | 30000;
  if (timeout < 5000 || timeout > 3600000) timeout = 30000;
  approval_deadline_ms = never ? 0 : millis() + timeout;
  approval_active = true;
  state_hold_until_ms = 0;
  audio.notifyEvent("permission_request");
  status_leds.setMode(StatusLedMode::Approval);
  board.renderApproval(approval_agent, approval_tool, approval_reason);
}

void sendHeartbeat() {
  JsonDocument payload;
  payload["runtime"] = kRuntime;
  payload["uptimeMs"] = millis();
  sendMessage("system/heartbeat", payload.as<JsonObjectConst>());
}

void handleLine(const String& line, const TransportContext& source) {
  DispatchGuard dispatch(source);
  JsonDocument request;
  const DeserializationError error = deserializeJson(request, line);
  if (error) {
    sendProtocolError(source, "invalid_json", "message must be one JSON object");
    return;
  }
  const char* topic = request["topic"] | "";
  const char* message_type = request["type"] | "";
  if (strlen(topic) > 96) {
    sendProtocolError(source, "invalid_topic", "topic exceeds 96 bytes");
    return;
  }
  JsonObjectConst payload = request["payload"].as<JsonObjectConst>();
  bool authenticated_now = false;
  if (wifi_transport.handleControl(topic, payload, source, authenticated_now)) {
    if (authenticated_now) sendHello(protocolTransport().owner());
    return;
  }

  // Native USB CDC does not reset the S3 when the desktop opens the port.
  // Every USB probe establishes a new owner generation before hello is sent.
  if (strcmp(topic, "bind") == 0 || strcmp(message_type, "hello") == 0) {
    if (source.kind == TransportKind::Usb) {
      const TransportContext claimed = protocolTransport().claimOwner(source, true);
      sendHello(claimed);
    } else if (protocolTransport().admitted(source)) {
      sendHello(source);
    } else {
      sendProtocolError(source, "authentication_required",
                        "Wi-Fi hello requires an authenticated connection");
    }
    return;
  }
  if (!protocolTransport().admitted(source)) {
    sendProtocolError(source, "connection_not_owner",
                      "message belongs to an unauthenticated or retired connection",
                      payload["requestId"] | "");
    return;
  }
  if (strcmp(topic, "system/heartbeat") == 0) return;
  if (power.handleRequest(topic, payload)) return;
  if (!strcmp(topic, "display/avatar_begin") && audio.playbackActive()) {
    JsonDocument avatar_ack;
    JsonObject ack = avatar_ack.to<JsonObject>();
    ack["requestId"] = payload["requestId"] | "";
    ack["requestTopic"] = topic;
    ack["boardDeviceId"] = boardDeviceId();
    ack["ok"] = false;
    ack["code"] = "resource_busy";
    ack["message"] = "avatar transfer cannot start during audio playback";
    sendMessage("display/avatar_ack", avatar_ack.as<JsonObjectConst>());
    return;
  }
  JsonDocument avatar_ack;
  if (board.handleAvatar(topic, payload, avatar_ack)) {
    sendMessage("display/avatar_ack", avatar_ack.as<JsonObjectConst>());
    return;
  }
  if (strcmp(topic, "approval/request") == 0) {
    beginApproval(request["payload"].as<JsonObjectConst>());
  } else if (strcmp(topic, "approval/cancel") == 0) {
    const String request_id = request["payload"]["requestId"] | "";
    if (approval_active && request_id == approval_request_id) {
      clearApproval();
      audio.notifyEvent("permission_expired");
      status_leds.setMode(ledModeForState(current_state));
      board.renderStatus(current_agent, current_state, current_detail);
    }
  }
  if (strcmp(topic, "input/config") == 0) {
    board.applyInputConfig(request["payload"].as<JsonObjectConst>());
    return;
  }
  if (strcmp(topic, "input/config-query") == 0) {
    board.sendInputConfigState(request["payload"].as<JsonObjectConst>());
    return;
  }
  if (strcmp(topic, "system/reset-inputs") == 0) {
    const char* request_id = request["payload"]["requestId"] | "";
    board.resetInputConfig(request_id);
    return;
  }
  if (strcmp(topic, "diagnostics/query") == 0) {
    sendDiagnostics(request["payload"].as<JsonObjectConst>());
    return;
  }
  if (strcmp(topic, "system/reboot") == 0) {
    sendActionResult(payload, "reboot", "设备正在重启");
    protocolTransport().flush();
    reboot_pending = true;
    return;
  }
  if (strcmp(topic, "control/command") == 0 && !strcmp(request["payload"]["type"] | "", "audio_bridge")) {
    const char* action = request["payload"]["action"] | "";
    audio_bridge_armed = !strcmp(action, "start");
    if (!audio_bridge_armed) {
      board.setVoiceCaptureActive(false);
      audio.stopCapture("bridge-stopped");
    }
    return;
  }
  const bool audio_topic = !strcmp(topic, "audio/volume") ||
                           !strcmp(topic, "audio/play_begin") ||
                           !strcmp(topic, "audio/play_chunk") ||
                           !strcmp(topic, "audio/play_end") ||
                           !strcmp(topic, "audio/play_flush") ||
                           !strcmp(topic, "audio/play_subtitle") ||
                           !strcmp(topic, "audio/conversation") ||
                           !strcmp(topic, "audio/query") ||
                           !strcmp(topic, "audio/control");
  const bool approval_topic = strcmp(topic, "approval/request") == 0 ||
                              strcmp(topic, "approval/cancel") == 0;
  const bool display_topic = strncmp(topic, "state/", 6) == 0 ||
                             strcmp(topic, "speech/text") == 0 ||
                             strcmp(topic, "session/current") == 0 ||
                             strcmp(topic, "ui/conversation") == 0;
  const bool known_topic = audio_topic || display_topic || approval_topic;
  if (audio_topic) {
    if (!strcmp(topic, "audio/play_begin") && board.avatarTransferActive())
      audio.rejectPlayback(payload, "playback cannot start during an avatar transfer");
    else
      audio.handle(topic, payload);
  }
  if (strncmp(topic, "state/", 6) == 0) {
    const String next_state = normalizedState(request["payload"]["state"] | "idle");
    current_agent = request["payload"]["source"] | (topic + 6);
    const char* detail = request["payload"]["displayContent"] | "";
    if (!*detail) detail = request["payload"]["statusText"] | "";
    if (!*detail) detail = request["payload"]["event"] | "";
    current_detail = detail;
    if (next_state != current_state) {
      if (next_state == "WAITING") audio.notifyEvent("waiting_user");
      else if (next_state == "DONE") audio.notifyEvent("done");
      else if (next_state == "ERROR") audio.notifyEvent("error");
    }
    current_state = next_state;
    if (next_state == "DONE")
      status_leds.showTemporary(StatusLedMode::Success, 5000);
    else if (next_state == "ERROR")
      status_leds.showTemporary(StatusLedMode::Error, 5000);
    else
      status_leds.setMode(ledModeForState(next_state));
    board.selectAvatarState(next_state.c_str());
    const bool notification_state = next_state == "WAITING" || next_state == "DONE" || next_state == "ERROR";
    if (notification_state) state_hold_until_ms = millis() + 5000;
    const bool state_held = static_cast<int32_t>(state_hold_until_ms - millis()) > 0;
    if (!approval_active && (notification_state || !state_held))
      board.renderStatus(current_agent, current_state, current_detail);
  } else if (strcmp(topic, "speech/text") == 0) {
    const String title = request["payload"]["displayTitle"] | "VOICE";
    const String body = request["payload"]["displayContent"] | "";
    board.renderVoice(title, body);
  } else if (strcmp(topic, "ui/conversation") == 0) {
    const String state = request["payload"]["state"] | "listening";
    const String name = request["payload"]["name"] | "";
    const String text = request["payload"]["text"] | "";
    board.renderConversation(state, name, text);
    if (state == "listening") status_leds.setMode(StatusLedMode::Recording);
    else if (state == "thinking" || state == "preparing") status_leds.setMode(StatusLedMode::Working);
    else if (state == "speaking") status_leds.setMode(StatusLedMode::Speaking);
    else if (state == "error") status_leds.showTemporary(StatusLedMode::Error, 3000);
    else if (state == "ended") status_leds.setMode(ledModeForState(current_state));
  } else if (strcmp(topic, "session/current") == 0) {
    current_detail = request["payload"]["title"] | "session";
    if (!approval_active) board.renderStatus(current_agent, current_state, current_detail);
  }
  if (!strcmp(topic, "audio/play_subtitle")) {
    const String text = request["payload"]["text"] | "";
    if (!text.isEmpty()) board.renderConversation("speaking", current_agent, text);
  }
  if (request["payload"]["requestId"].is<const char*>()) {
    JsonDocument ack;
    ack["requestId"] = request["payload"]["requestId"];
    ack["requestTopic"] = topic;
    if (request["payload"]["sessionId"].is<const char*>())
      ack["sessionId"] = request["payload"]["sessionId"];
    if (!strcmp(topic, "audio/conversation"))
      ack["conversationSessionId"] = request["payload"]["sessionId"] | "";
    ack["ok"] = known_topic;
    ack["stage"] = "received";
    ack["completed"] = false;
    ack["code"] = known_topic ? "received" : "not_implemented";
    sendMessage("protocol/ack", ack.as<JsonObjectConst>());
  }
}
}

void setup() {
  char boot_text[17];
  snprintf(boot_text, sizeof(boot_text), "%08lx%08lx",
           static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()));
  boot_id = boot_text;
  wifi_transport.setIdentity(boardDeviceId(), boot_id);
  protocolTransport().begin(usb_transport, wifi_transport,
                            onTransportOwnerChanged);
  delay(200);
  power.begin();
  board.begin();
  board.setInputHandler(handleInputAction);
  audio.begin();
  status_leds.begin();
  sendHello();
}

void loop() {
  static uint32_t lastHeartbeat = 0;
  static uint32_t previousLoopAt = 0;
  const uint32_t loopAt = millis();
  if (previousLoopAt != 0)
    max_loop_gap_ms = max(max_loop_gap_ms, loopAt - previousLoopAt);
  previousLoopAt = loopAt;
  protocolTransport().poll();
  TransportContext source;
  String message;
  for (uint8_t budget = 0; budget < 4 &&
       protocolTransport().nextInbound(source, message); ++budget) {
    handleLine(message, source);
    message = "";
  }
  protocolTransport().poll();
  board.poll();
  power.poll();
  audio.poll();
  status_leds.poll();
  if (approval_active && approval_deadline_ms != 0 &&
      static_cast<int32_t>(millis() - approval_deadline_ms) >= 0) {
    clearApproval();
    audio.notifyEvent("permission_expired");
    status_leds.setMode(ledModeForState(current_state));
    board.renderStatus(current_agent, current_state, current_detail);
  }
  const bool state_held = static_cast<int32_t>(state_hold_until_ms - millis()) > 0;
  if (!approval_active && !state_held) board.renderStatus(current_agent, current_state, current_detail);
  if (millis() - lastHeartbeat >= kHeartbeatMs) {
    lastHeartbeat = millis();
    sendHeartbeat();
  }
  protocolTransport().poll();
  if (reboot_pending) {
    protocolTransport().flush();
    delay(100);
    ESP.restart();
  }
  delay(2);
}

