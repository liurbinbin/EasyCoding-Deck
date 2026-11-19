#include "status_led_service.h"

#include <esp32-hal-rmt.h>
#include <cstring>

#include "pin_config.h"

namespace {
constexpr uint8_t kLedCount = 14;
constexpr uint32_t kFrameMs = 50;
}

void StatusLedService::begin() {
  ready_ = rmtInit(tkeyboard::pins::ws2812, RMT_TX_MODE,
                   RMT_MEM_NUM_BLOCKS_1, 10000000);
  clear();
  write();
}

void StatusLedService::setMode(StatusLedMode mode) {
  mode_ = mode;
  restore_mode_ = mode;
  temporary_until_ = 0;
  dirty_ = true;
}

void StatusLedService::showTemporary(StatusLedMode mode, uint32_t duration_ms,
                                     StatusLedMode restore) {
  mode_ = mode;
  restore_mode_ = restore;
  temporary_until_ = millis() + duration_ms;
  dirty_ = true;
}

void StatusLedService::clear() {
  memset(colors_, 0, sizeof(colors_));
}

void StatusLedService::pixel(uint8_t index, uint8_t red, uint8_t green, uint8_t blue) {
  if (index >= kLedCount) return;
  colors_[index][0] = red;
  colors_[index][1] = green;
  colors_[index][2] = blue;
}

void StatusLedService::write() {
  if (!ready_) return;
  rmt_data_t symbols[kLedCount * 24];
  size_t symbol = 0;
  for (uint8_t led = 0; led < kLedCount; ++led) {
    const uint8_t grb[] = {colors_[led][1], colors_[led][0], colors_[led][2]};
    for (uint8_t component : grb) {
      for (int bit = 7; bit >= 0; --bit) {
        const bool one = component & (1U << bit);
        symbols[symbol].level0 = 1;
        symbols[symbol].duration0 = one ? 8 : 4;
        symbols[symbol].level1 = 0;
        symbols[symbol].duration1 = one ? 4 : 8;
        ++symbol;
      }
    }
  }
  rmtWrite(tkeyboard::pins::ws2812, symbols, symbol, RMT_WAIT_FOR_EVER);
  delayMicroseconds(80);
}

void StatusLedService::render(uint32_t step) {
  clear();
  switch (mode_) {
    case StatusLedMode::Idle:
      break;
    case StatusLedMode::Recording: {
      const uint8_t phase = (step / 2) % 9;
      if (phase <= 6) {
        const auto ring = [this](uint8_t radius, uint8_t level) {
          pixel(6 - radius, 0, level, 0);
          pixel(7 + radius, 0, level, 0);
        };
        ring(phase, 42);
        if (phase > 0) ring(phase - 1, 16);
        if (phase > 1) ring(phase - 2, 5);
      }
      break;
    }
    case StatusLedMode::Working: {
      const uint8_t phase = (step / 2) % 82;
      if (phase < 27) {
        const int position = phase <= 13 ? phase : 26 - phase;
        const int direction = phase < 13 ? 1 : -1;
        const uint8_t levels[] = {42, 18, 6};
        for (uint8_t trail = 0; trail < 3; ++trail) {
          const int index = position - direction * trail;
          if (index >= 0 && index < kLedCount)
            pixel(index, 0, static_cast<uint8_t>(levels[trail] / 2), levels[trail]);
        }
      } else if (phase < 41) {
        const uint8_t radius = (phase - 27) / 2;
        pixel(radius, 0, 20, 42);
        pixel(kLedCount - 1 - radius, 0, 20, 42);
      } else if (phase < 55) {
        const uint8_t radius = (phase - 41) / 2;
        pixel(6 - radius, 0, 20, 42);
        pixel(7 + radius, 0, 20, 42);
      } else {
        const uint8_t travel = phase - 55;
        const uint8_t position = travel <= 13 ? travel : 26 - travel;
        pixel(position, 0, 28, 42);
        pixel(kLedCount - 1 - position, 0, 12, 32);
      }
      break;
    }
    case StatusLedMode::Speaking:
      for (uint8_t index = 0; index < kLedCount; ++index) {
        const uint8_t level = static_cast<uint8_t>(8 + ((index * 5 + step * 7) % 25));
        pixel(index, static_cast<uint8_t>(level / 2), level, 28);
      }
      break;
    case StatusLedMode::Waiting: {
      const uint8_t level = static_cast<uint8_t>(8 + (step % 12) * 2);
      for (uint8_t index = 0; index < kLedCount; ++index) pixel(index, level, level / 3, 0);
      break;
    }
    case StatusLedMode::Approval:
      for (uint8_t index = 0; index < kLedCount; ++index) {
        if (((index / 2) + (step / 3)) % 2 == 0) pixel(index, 32, 10, 0);
      }
      break;
    case StatusLedMode::Success: {
      uint8_t filled = static_cast<uint8_t>((step % 18) + 1);
      if (filled > kLedCount) filled = kLedCount;
      for (uint8_t index = 0; index < filled; ++index) pixel(index, 0, 32, 8);
      break;
    }
    case StatusLedMode::Error:
      if ((step / 3) % 2 == 0)
        for (uint8_t index = 0; index < kLedCount; ++index) pixel(index, 36, 0, 0);
      break;
  }
}

void StatusLedService::poll() {
  if (!ready_) return;
  const uint32_t now = millis();
  if (temporary_until_ && static_cast<int32_t>(now - temporary_until_) >= 0) {
    mode_ = restore_mode_;
    temporary_until_ = 0;
    dirty_ = true;
  }
  if (!dirty_ && now - last_frame_at_ < kFrameMs) return;
  const uint32_t step = now / kFrameMs;
  if (!dirty_ && step == last_step_) return;
  last_frame_at_ = now;
  last_step_ = step;
  dirty_ = false;
  render(step);
  write();
}
