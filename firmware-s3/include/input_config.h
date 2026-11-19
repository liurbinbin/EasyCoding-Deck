#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

class InputConfig {
 public:
  static constexpr size_t kMaxBindings = 20;

  void begin();
  bool apply(JsonObjectConst payload);
  void reset(const char* request_id);
  void sendState(JsonObjectConst request) const;
  const char* actionFor(const char* event) const;
  const char* valueFor(const char* event) const;
  int shortKeyForAction(const char* action) const;
  int approvalDecisionForShortEvent(const char* event) const;
  size_t bindingCount() const { return binding_count_; }

 private:
  struct Binding {
    String event;
    String action;
    String value;
  };

  void loadDefaults();
  bool loadPersisted();
  bool persist() const;
  bool setBinding(const char* event, const char* action, const char* value);
  void sendAck(const char* request_id, bool ok, const char* message) const;
  static bool validEvent(const char* event);
  static bool validAction(const char* action);

  Binding bindings_[kMaxBindings];
  size_t binding_count_ = 0;
};
