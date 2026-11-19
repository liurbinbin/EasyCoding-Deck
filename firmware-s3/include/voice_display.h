#pragma once

#include <LovyanGFX.hpp>

class VoiceDisplay final : public lgfx::LGFX_Device {
 public:
  bool begin(lgfx::Bus_SPI& bus, int cs_pin);
  bool initialized() const { return initialized_; }
  void drawFullFrame(const uint16_t* pixels);

 private:
  lgfx::Panel_GC9107 panel_;
  bool initialized_ = false;
};

