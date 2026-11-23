#include "power_manager.h"

#include <ArduinoJson.h>
#include <PmicDrv.hpp>
#include <Preferences.h>
#include <cstring>
#include <Wire.h>
#include <pmic/xpowers/axp2101/AXP2101Regs.hpp>
#include <esp_sleep.h>

#include "pin_config.h"
#include "protocol_transport.h"

namespace {
constexpr uint8_t kAxp2101 = 0x34;
constexpr char kPowerNamespace[] = "power-key";
constexpr char kHoldSecondsKey[] = "hold-sec";
constexpr char kActionKey[] = "action";
constexpr uint8_t kDefaultHoldSeconds = 3;
constexpr uint8_t kMinHoldSeconds = 2;
constexpr uint8_t kMaxHoldSeconds = 10;
PmicAXP2101 pmic;

String boardDeviceId() {
  return String("s3-tkeyboard-") + String((uint32_t)ESP.getEfuseMac(), HEX);
}

bool readRegister(uint8_t reg, uint8_t& value) {
  Wire.beginTransmission(kAxp2101);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(kAxp2101, static_cast<uint8_t>(1)) != 1) return false;
  value = Wire.read();
  return true;
}

bool writeRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(kAxp2101);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}
}

void PowerManager::begin() {
  Preferences preferences;
  if (preferences.begin(kPowerNamespace, true)) {
    const uint8_t seconds = preferences.getUChar(kHoldSecondsKey, kDefaultHoldSeconds);
    const uint8_t action = preferences.getUChar(kActionKey, 0);
    preferences.end();
    hold_ms_ = constrain(seconds, kMinHoldSeconds, kMaxHoldSeconds) * 1000UL;
    action_ = action == static_cast<uint8_t>(Key4Action::DeepSleep)
        ? Key4Action::DeepSleep : Key4Action::PowerOff;
  }
  Wire.begin(tkeyboard::pins::i2c_sda, tkeyboard::pins::i2c_scl, 400000);
  uint8_t ts_control = 0;
  uint8_t adc_control = 0;
  const bool preserved = readRegister(axp2101_regs::ts::TS_PIN_CTRL, ts_control) &&
                         readRegister(axp2101_regs::adc::CHANNEL_CTRL, adc_control);
  pmic_available_ = preserved && pmic.begin(Wire, kAxp2101,
      tkeyboard::pins::i2c_sda, tkeyboard::pins::i2c_scl);
  Wire.setClock(400000);
  if (pmic_available_) {
    pmic_available_ = writeRegister(axp2101_regs::ts::TS_PIN_CTRL, ts_control) &&
                      writeRegister(axp2101_regs::adc::CHANNEL_CTRL, adc_control);
  }
  display_rail_available_ = pmic_available_ && configureDisplayRail();
  if (pmic_available_) configurePowerKeySafety();
  pinMode(tkeyboard::pins::key4, INPUT_PULLUP);
  pinMode(tkeyboard::pins::pmic_irq, INPUT_PULLUP);
  publishStatus();
}

bool PowerManager::configurePowerKeySafety() {
  // KEY4 is also AXP2101 PWRON. Disable the PMIC's fixed long-press action so
  // firmware can honor the configured duration and choose PMIC off or sleep.
  uint8_t common = 0;
  const bool common_ready = readRegister(axp2101_regs::ctrl::COMMON_CFG, common) &&
      writeRegister(axp2101_regs::ctrl::COMMON_CFG, static_cast<uint8_t>(common & 0xF0));
  return writeRegister(axp2101_regs::pmu::PWROFF_EN, 0x04) &&
      writeRegister(axp2101_regs::pmu::SLEEP_WAKEUP_CTRL, 0x00) &&
      pmic.pwron().setOnDurationMs(512) && pmic.pwron().setOffDurationMs(6000) &&
      common_ready;
}

bool PowerManager::persistConfig(uint8_t seconds, Key4Action action) {
  Preferences preferences;
  if (!preferences.begin(kPowerNamespace, false)) return false;
  const bool seconds_ok = preferences.putUChar(kHoldSecondsKey, seconds) == sizeof(uint8_t);
  const bool action_ok = preferences.putUChar(kActionKey, static_cast<uint8_t>(action)) == sizeof(uint8_t);
  preferences.end();
  if (seconds_ok && action_ok) {
    hold_ms_ = seconds * 1000UL;
    action_ = action;
  }
  return seconds_ok && action_ok;
}

const char* PowerManager::actionName() const {
  return action_ == Key4Action::DeepSleep ? "deep_sleep" : "power_off";
}

bool PowerManager::configureDisplayRail() {
  auto* rails = pmic.getChannel();
  if (!rails) return false;
  bool ready = true;
  for (uint8_t channel : {AXP2101Channel::CH_DCDC1, AXP2101Channel::CH_ALDO2,
                          AXP2101Channel::CH_ALDO3, AXP2101Channel::CH_DLDO1}) {
    ready = rails->setVoltage(channel, 3300) && rails->enable(channel, true) && ready;
    ready = rails->isEnabled(channel) && rails->getVoltage(channel) == 3300 && ready;
  }
  ready = pmic.enableModule(PmicAXP2101::Module::WATCHDOG, false) && ready;
  ready = pmic.enableModule(PmicAXP2101::Module::GAUGE, true) && ready;
  ready = pmic.enableModule(PmicAXP2101::Module::BAT_DETECT, true) && ready;
  const auto adc_channels = PmicAdcBase::Channel::BAT_VOLTAGE |
                            PmicAdcBase::Channel::VBUS_VOLTAGE |
                            PmicAdcBase::Channel::VSYS_VOLTAGE;
  ready = pmic.adc().enableChannels(adc_channels) && ready;
  delay(20);
  return ready;
}

void PowerManager::sampleTelemetry() {
  if (!pmic_available_) return;
  auto* charger = pmic.getCharger();
  if (!charger) return;
  const auto status = charger->getStatus();
  battery_present_ = status.batteryPresent;
  vbus_present_ = status.vbusPresent;
  charging_ = status.charging;
  float value = 0;
  battery_percent_ = pmic.adc().read(PmicAdcBase::Channel::BAT_PERCENTAGE, value)
      ? static_cast<int>(value + 0.5f) : -1;
  battery_mv_ = pmic.adc().read(PmicAdcBase::Channel::BAT_VOLTAGE, value)
      ? static_cast<int>(value + 0.5f) : -1;
}

void PowerManager::publishStatus(const char* request_id) {
  sampleTelemetry();
  JsonDocument doc;
  doc["topic"] = "power/status";
  auto payload = doc["payload"].to<JsonObject>();
  payload["boardDeviceId"] = boardDeviceId();
  payload["pmic"] = "AXP2101";
  payload["pmicAvailable"] = pmic_available_;
  payload["displayRailAvailable"] = display_rail_available_;
  payload["batteryPresent"] = battery_present_;
  payload["vbusPresent"] = vbus_present_;
  payload["charging"] = charging_;
  if (battery_percent_ >= 0) payload["batteryPercent"] = battery_percent_;
  if (battery_mv_ >= 0) payload["batteryMillivolts"] = battery_mv_;
  payload["deepSleep"] = true;
  payload["powerOff"] = pmic_available_;
  payload["key4HoldSeconds"] = hold_ms_ / 1000UL;
  payload["key4Action"] = actionName();
  payload["configurableHold"] = true;
  if (request_id && *request_id) payload["requestId"] = request_id;
  payload["key4"] = digitalRead(tkeyboard::pins::key4) == LOW ? "pressed" : "released";
  payload["pmicIrq"] = digitalRead(tkeyboard::pins::pmic_irq) == LOW;
  payload["uptimeMs"] = millis();
  protocolTransport().sendDocument(doc);
}

void PowerManager::sendConfigAck(const char* request_id, bool ok, const char* message) {
  JsonDocument doc;
  doc["topic"] = "power/config-ack";
  JsonObject payload = doc["payload"].to<JsonObject>();
  payload["requestId"] = request_id ? request_id : "";
  payload["boardDeviceId"] = boardDeviceId();
  payload["ok"] = ok;
  payload["key4HoldSeconds"] = hold_ms_ / 1000UL;
  payload["key4Action"] = actionName();
  payload["message"] = message;
  protocolTransport().sendDocument(doc);
}

bool PowerManager::handleRequest(const char* topic, JsonObjectConst payload) {
  const char* request_id = payload["requestId"] | "";
  if (!strcmp(topic, "power/config-query")) {
    publishStatus(request_id);
    return true;
  }
  if (strcmp(topic, "power/config")) return false;
  const int seconds = payload["key4HoldSeconds"] | 0;
  if (seconds < kMinHoldSeconds || seconds > kMaxHoldSeconds) {
    sendConfigAck(request_id, false, "hold seconds must be between 2 and 10");
    return true;
  }
  const char* requested_action = payload["key4Action"] | "power_off";
  Key4Action action;
  if (!strcmp(requested_action, "power_off")) {
    action = Key4Action::PowerOff;
  } else if (!strcmp(requested_action, "deep_sleep")) {
    action = Key4Action::DeepSleep;
  } else {
    sendConfigAck(request_id, false, "key4 action must be power_off or deep_sleep");
    return true;
  }
  if (action == Key4Action::PowerOff && !pmic_available_) {
    sendConfigAck(request_id, false, "AXP2101 unavailable; power off cannot be configured");
    return true;
  }
  const bool ok = persistConfig(static_cast<uint8_t>(seconds), action);
  sendConfigAck(request_id, ok, ok ? "power key config written" : "persist power key config failed");
  if (ok) publishStatus();
  return true;
}

void PowerManager::runArmedAction() {
  if (action_ == Key4Action::DeepSleep) {
    esp_sleep_enable_ext1_wakeup(1ULL << tkeyboard::pins::key4, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();
    return;
  }
  if (!pmic_available_) return;
  protocolTransport().flush();
  delay(30);
  pmic.shutdown();
}

void PowerManager::poll() {
  const uint32_t now = millis();
  const bool pressed = digitalRead(tkeyboard::pins::key4) == LOW;
  if (pressed && key_pressed_at_ == 0) key_pressed_at_ = now;
  if (pressed && !action_armed_ && key_pressed_at_ != 0 && now - key_pressed_at_ >= hold_ms_) {
    action_armed_ = true;
    publishStatus();
  }
  if (!pressed && action_armed_) {
    // Run after release so deep sleep cannot immediately wake and PMIC off
    // cannot interpret the same held PWRON level as a new power-on request.
    delay(50);
    runArmedAction();
  }
  if (!pressed) {
    key_pressed_at_ = 0;
    action_armed_ = false;
  }
  if (now - last_status_ >= 5000) {
    last_status_ = now;
    publishStatus();
  }
}

