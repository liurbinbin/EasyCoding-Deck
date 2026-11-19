#pragma once

#include <Arduino.h>

enum class StatusLedMode : uint8_t {
  Idle,
  Recording,
  Working,
  Speaking,
  Waiting,
  Approval,
  Success,
  Error,
};

class StatusLedService {
 public:
  void begin();
  void poll();
  void setMode(StatusLedMode mode);
  void showTemporary(StatusLedMode mode, uint32_t duration_ms,
                     StatusLedMode restore = StatusLedMode::Idle);

 private:
  void render(uint32_t step);
  void write();
  void clear();
  void pixel(uint8_t index, uint8_t red, uint8_t green, uint8_t blue);

  bool ready_ = false;
  bool dirty_ = true;
  StatusLedMode mode_ = StatusLedMode::Idle;
  StatusLedMode restore_mode_ = StatusLedMode::Idle;
  uint32_t temporary_until_ = 0;
  uint32_t last_frame_at_ = 0;
  uint32_t last_step_ = UINT32_MAX;
  uint8_t colors_[14][3] = {};
};
