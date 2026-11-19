#include "voice_display.h"

bool VoiceDisplay::begin(lgfx::Bus_SPI& bus, int cs_pin) {
  panel_.setBus(&bus);
  auto config = panel_.config();
  config.pin_cs = cs_pin;
  config.pin_rst = -1;
  config.pin_busy = -1;
  config.memory_width = 128;
  config.memory_height = 128;
  config.panel_width = 128;
  config.panel_height = 128;
  config.offset_y = 32;
  config.readable = false;
  config.bus_shared = true;
  panel_.config(config);
  setPanel(&panel_);
  if (!init()) return false;
  setRotation(0);
  fillScreen(TFT_BLACK);
  initialized_ = true;
  return true;
}

void VoiceDisplay::drawFullFrame(const uint16_t* pixels) {
  if (!initialized_ || !pixels) return;
  startWrite();
  setAddrWindow(0, 0, 128, 128);
  writePixels(pixels, 128 * 128, true);
  endWrite();
}

