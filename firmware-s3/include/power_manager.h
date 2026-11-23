#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

class PowerManager {
 public:
  void begin();
  void poll();
  bool handleRequest(const char* topic, JsonObjectConst payload);
  bool pmicAvailable() const { return pmic_available_; }
  bool railsAvailable() const { return display_rail_available_; }
  bool batteryPresent() const { return battery_present_; }
  bool vbusPresent() const { return vbus_present_; }
  bool charging() const { return charging_; }
  int batteryPercent() const { return battery_percent_; }
  int batteryMillivolts() const { return battery_mv_; }

 private:
  enum class Key4Action : uint8_t { PowerOff = 0, DeepSleep = 1 };

  bool configureDisplayRail();
  bool configurePowerKeySafety();
  bool persistConfig(uint8_t seconds, Key4Action action);
  const char* actionName() const;
  void runArmedAction();
  void sampleTelemetry();
  void publishStatus(const char* request_id = "");
  void sendConfigAck(const char* request_id, bool ok, const char* message);
  bool pmic_available_ = false;
  bool display_rail_available_ = false;
  bool action_armed_ = false;
  Key4Action action_ = Key4Action::PowerOff;
  uint32_t key_pressed_at_ = 0;
  uint32_t last_status_ = 0;
  uint32_t hold_ms_ = 3000;
  bool battery_present_ = false;
  bool vbus_present_ = false;
  bool charging_ = false;
  int battery_percent_ = -1;
  int battery_mv_ = -1;
};

