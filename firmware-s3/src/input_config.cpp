#include "input_config.h"

#include <Preferences.h>
#include <cstring>

#include "protocol_transport.h"

namespace {
constexpr char kNamespace[] = "input-map";
constexpr char kStorageKey[] = "bindings";

void sendDocument(JsonDocument& doc) {
  protocolTransport().sendDocument(doc);
}
}

bool InputConfig::validEvent(const char* event) {
  if (!event) return false;
  for (int key = 1; key <= 5; ++key) {
    char name[32];
    for (const char* gesture : {"short_press", "long_press", "hold"}) {
      snprintf(name, sizeof(name), "button.sw%d.%s", key, gesture);
      if (!strcmp(event, name)) return true;
    }
  }
  return false;
}

bool InputConfig::validAction(const char* action) {
  static const char* allowed[] = {
      "disabled", "voice_ptt", "realtime_chat", "agent_enter", "agent_prompt",
      "session_next", "session_previous", "session_clear", "dsh_interrupt",
      "component_center", "page_enter", "page_back"};
  if (!action) return false;
  for (const char* candidate : allowed) if (!strcmp(action, candidate)) return true;
  return false;
}

bool InputConfig::setBinding(const char* event, const char* action, const char* value) {
  if (!validEvent(event) || !validAction(action) || strlen(value ? value : "") >= 160) return false;
  // KEY4 hold belongs to the power manager and must never be remapped by PC config.
  if ((!strcmp(event, "button.sw4.long_press") || !strcmp(event, "button.sw4.hold")) &&
      strcmp(action, "disabled")) return false;
  const String event_name(event);
  const bool exclusive_approval_action = event_name.endsWith(".short_press") &&
      (!strcmp(action, "page_enter") || !strcmp(action, "page_back"));
  if (exclusive_approval_action) {
    for (size_t i = 0; i < binding_count_; ++i) {
      if (bindings_[i].event != event && bindings_[i].event.endsWith(".short_press") &&
          bindings_[i].action == action) {
        bindings_[i].action = "disabled";
        bindings_[i].value = "";
      }
    }
  }
  size_t index = binding_count_;
  for (size_t i = 0; i < binding_count_; ++i) {
    if (bindings_[i].event == event) { index = i; break; }
  }
  if (index == binding_count_) {
    if (binding_count_ >= kMaxBindings) return false;
    ++binding_count_;
  }
  bindings_[index] = {event, action, value ? value : ""};
  return true;
}

void InputConfig::loadDefaults() {
  binding_count_ = 0;
  const char* defaults[][3] = {
      {"button.sw1.short_press", "page_enter", ""},
      {"button.sw1.long_press", "disabled", ""},
      {"button.sw1.hold", "voice_ptt", ""},
      {"button.sw2.short_press", "session_previous", ""},
      {"button.sw2.long_press", "disabled", ""},
      {"button.sw2.hold", "disabled", ""},
      {"button.sw3.short_press", "session_next", ""},
      {"button.sw3.long_press", "disabled", ""},
      {"button.sw3.hold", "disabled", ""},
      {"button.sw4.short_press", "page_back", ""},
      {"button.sw4.long_press", "disabled", ""},
      {"button.sw4.hold", "disabled", ""},
      {"button.sw5.short_press", "component_center", ""},
      {"button.sw5.long_press", "realtime_chat", ""},
      {"button.sw5.hold", "disabled", ""},
  };
  for (const auto& row : defaults) setBinding(row[0], row[1], row[2]);
}

bool InputConfig::loadPersisted() {
  Preferences preferences;
  if (!preferences.begin(kNamespace, true)) return false;
  const String json = preferences.getString(kStorageKey, "");
  preferences.end();
  if (json.isEmpty()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  JsonArrayConst rows = doc["bindings"].as<JsonArrayConst>();
  if (rows.isNull()) return false;
  loadDefaults();
  for (JsonObjectConst row : rows) {
    if (!setBinding(row["event"] | "", row["action"] | "", row["value"] | "")) return false;
  }
  return true;
}

bool InputConfig::persist() const {
  JsonDocument doc;
  JsonArray rows = doc["bindings"].to<JsonArray>();
  for (size_t i = 0; i < binding_count_; ++i) {
    JsonObject row = rows.add<JsonObject>();
    row["event"] = bindings_[i].event;
    row["action"] = bindings_[i].action;
    row["value"] = bindings_[i].value;
  }
  String json;
  serializeJson(doc, json);
  Preferences preferences;
  if (!preferences.begin(kNamespace, false)) return false;
  const bool ok = preferences.putString(kStorageKey, json) == json.length();
  preferences.end();
  return ok;
}

void InputConfig::begin() {
  if (!loadPersisted()) loadDefaults();
}

void InputConfig::sendAck(const char* request_id, bool ok, const char* message) const {
  JsonDocument doc;
  doc["topic"] = "input/config-ack";
  JsonObject payload = doc["payload"].to<JsonObject>();
  payload["requestId"] = request_id ? request_id : "";
  payload["ok"] = ok;
  payload["version"] = 1;
  payload["bindingCount"] = binding_count_;
  payload["persisted"] = ok;
  payload["message"] = message;
  sendDocument(doc);
}

bool InputConfig::apply(JsonObjectConst payload) {
  const char* request_id = payload["requestId"] | "";
  if (*request_id == '\0') request_id = payload["request_id"] | "";
  JsonArrayConst rows = payload["bindings"].as<JsonArrayConst>();
  if (rows.isNull() || rows.size() > kMaxBindings) {
    sendAck(request_id, false, "invalid bindings array");
    return false;
  }
  InputConfig next;
  next.loadDefaults();
  for (JsonObjectConst row : rows) {
    if (!next.setBinding(row["event"] | "", row["action"] | "", row["value"] | "")) {
      sendAck(request_id, false, "invalid input binding");
      return false;
    }
  }
  if (!next.persist()) {
    sendAck(request_id, false, "persist input config failed");
    return false;
  }
  *this = next;
  sendAck(request_id, true, "input config written");
  return true;
}

void InputConfig::reset(const char* request_id) {
  loadDefaults();
  const bool ok = persist();
  JsonDocument doc;
  doc["topic"] = "diagnostics/action";
  JsonObject payload = doc["payload"].to<JsonObject>();
  payload["requestId"] = request_id ? request_id : "";
  payload["ok"] = ok;
  payload["action"] = "reset-inputs";
  payload["message"] = ok ? "按键配置已恢复为 T-Keyboard Voice 默认值" : "按键配置保存失败";
  sendDocument(doc);
}

void InputConfig::sendState(JsonObjectConst request) const {
  JsonDocument doc;
  doc["topic"] = "input/config-state";
  JsonObject payload = doc["payload"].to<JsonObject>();
  payload["requestId"] = request["requestId"] | "";
  payload["ok"] = true;
  payload["boardDeviceId"] = String("s3-tkeyboard-") + String((uint32_t)ESP.getEfuseMac(), HEX);
  payload["runtime"] = "esp32-s3-tkeyboard";
  payload["bindingCount"] = binding_count_;
  JsonObject config = payload["config"].to<JsonObject>();
  config["version"] = 1;
  JsonArray rows = config["bindings"].to<JsonArray>();
  const char* voice_button = "";
  for (size_t i = 0; i < binding_count_; ++i) {
    JsonObject row = rows.add<JsonObject>();
    row["event"] = bindings_[i].event;
    row["action"] = bindings_[i].action;
    row["value"] = bindings_[i].value;
    if (!strcmp(bindings_[i].action.c_str(), "voice_ptt")) voice_button = bindings_[i].event.c_str();
  }
  config["voiceEnabled"] = *voice_button != '\0';
  config["voiceButton"] = voice_button;
  sendDocument(doc);
}

const char* InputConfig::actionFor(const char* event) const {
  for (size_t i = 0; i < binding_count_; ++i) if (bindings_[i].event == event) return bindings_[i].action.c_str();
  return "disabled";
}

const char* InputConfig::valueFor(const char* event) const {
  for (size_t i = 0; i < binding_count_; ++i) if (bindings_[i].event == event) return bindings_[i].value.c_str();
  return "";
}

int InputConfig::shortKeyForAction(const char* action) const {
  if (!action || !*action) return 0;
  for (int key = 1; key <= 5; ++key) {
    char event[32];
    snprintf(event, sizeof(event), "button.sw%d.short_press", key);
    if (!strcmp(actionFor(event), action)) return key;
  }
  return 0;
}

int InputConfig::approvalDecisionForShortEvent(const char* event) const {
  if (!event || !String(event).endsWith(".short_press")) return 0;
  const char* action = actionFor(event);
  if (!strcmp(action, "page_enter")) return 1;
  if (!strcmp(action, "page_back")) return -1;
  if (shortKeyForAction("page_enter") == 0 && !strcmp(event, "button.sw1.short_press")) return 1;
  if (shortKeyForAction("page_back") == 0 && !strcmp(event, "button.sw4.short_press")) return -1;
  return 0;
}
