#include "board_runtime.h"

#include <ArduinoJson.h>
#include <esp_system.h>
#include <math.h>

#include "pin_config.h"
#include "protocol_transport.h"

namespace {
constexpr int kKeys[] = {tkeyboard::pins::key1, tkeyboard::pins::key2,
                         tkeyboard::pins::key3, tkeyboard::pins::key4,
                         tkeyboard::pins::key5};
constexpr uint32_t kDebounceMs = 35;
constexpr uint32_t kHoldMs = 500;
constexpr uint16_t kBackground = 0x0021;
constexpr uint16_t kSurface = 0x0861;
constexpr uint16_t kMuted = 0x8410;
constexpr uint16_t kCyan = 0x05ff;
constexpr uint16_t kAmber = 0xfd20;
constexpr uint16_t kGreen = 0x47e8;
constexpr uint16_t kRed = 0xf945;
constexpr int16_t kPanelTextLeft = 11;
constexpr int16_t kPanelTextWidth = 106;
constexpr uint32_t kEdgeLightFrameMs = 55;
constexpr int kEdgeHalfLength = 230;

uint16_t scale565(uint16_t color, uint8_t brightness) {
  const uint16_t r = ((color >> 11) & 0x1f) * brightness / 255;
  const uint16_t g = ((color >> 5) & 0x3f) * brightness / 255;
  const uint16_t b = (color & 0x1f) * brightness / 255;
  return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

void edgePoint(int distance, bool right, int16_t& x, int16_t& y) {
  distance = constrain(distance, 0, kEdgeHalfLength);
  if (distance <= 45) {
    x = static_cast<int16_t>(64 - distance);
    y = 3;
  } else if (distance <= 70) {
    const float t = static_cast<float>(distance - 45) / 25.0f;
    const float angle = -PI / 2.0f - t * PI / 2.0f;
    x = static_cast<int16_t>(lroundf(19.0f + cosf(angle) * 16.0f));
    y = static_cast<int16_t>(lroundf(19.0f + sinf(angle) * 16.0f));
  } else if (distance <= 160) {
    x = 3;
    y = static_cast<int16_t>(19 + distance - 70);
  } else if (distance <= 185) {
    const float t = static_cast<float>(distance - 160) / 25.0f;
    const float angle = PI - t * PI / 2.0f;
    x = static_cast<int16_t>(lroundf(19.0f + cosf(angle) * 16.0f));
    y = static_cast<int16_t>(lroundf(109.0f + sinf(angle) * 16.0f));
  } else {
    x = static_cast<int16_t>(19 + distance - 185);
    y = 125;
  }
  if (right) x = static_cast<int16_t>(127 - x);
}
}

void BoardRuntime::begin() {
  input_config_.begin();
  avatar_.begin();
  // GPIO1 is an active-low shared backlight. Keep it off until all panel
  // initialization commands have completed.
  pinMode(tkeyboard::pins::lcd_backlight, OUTPUT);
  digitalWrite(tkeyboard::pins::lcd_backlight, HIGH);
  for (int cs : tkeyboard::pins::lcd_cs) {
    pinMode(cs, OUTPUT);
    digitalWrite(cs, HIGH);
  }
  pinMode(tkeyboard::pins::lcd_rst, OUTPUT);
  digitalWrite(tkeyboard::pins::lcd_rst, LOW);
  delay(20);
  digitalWrite(tkeyboard::pins::lcd_rst, HIGH);
  delay(120);
  auto config = bus_.config();
  config.spi_host = SPI2_HOST;
  config.spi_mode = 0;
  config.freq_write = 4500000;
  config.freq_read = 4500000;
  config.spi_3wire = false;
  config.use_lock = true;
  config.pin_sclk = tkeyboard::pins::lcd_clk;
  config.pin_mosi = tkeyboard::pins::lcd_mosi;
  config.pin_miso = -1;
  config.pin_dc = tkeyboard::pins::lcd_dc;
  bus_.config(config);
  bus_.init();
  display_ready_ = true;
  for (size_t i = 0; i < 5; ++i) {
    display_ready_ = displays_[i].begin(bus_, tkeyboard::pins::lcd_cs[i]) && display_ready_;
  }
  if (display_ready_) {
    const char* labels[] = {"AVATAR", "ACTION", "MESSAGE", "CONTROLS", "STATUS"};
    for (size_t i = 0; i < 5; ++i) {
      drawPanel(displays_[i], kCyan, labels[i], i == 0 ? "HACHIMI" : "READY");
    }
    digitalWrite(tkeyboard::pins::lcd_backlight, LOW);
  }
  for (int pin : kKeys) pinMode(pin, INPUT_PULLUP);
}

void BoardRuntime::emitInput(const char* event, const char* gesture, int value) {
  const char* action = input_config_.actionFor(event);
  const char* binding_value = input_config_.valueFor(event);
  const bool handled_locally = input_handler_ && input_handler_(event, action, gesture);
  JsonDocument doc;
  doc["topic"] = "input/event";
  auto payload = doc["payload"].to<JsonObject>();
  payload["version"] = 1;
  payload["boardDeviceId"] = String("s3-tkeyboard-") + String((uint32_t)ESP.getEfuseMac(), HEX);
  payload["event"] = event;
  payload["gesture"] = gesture;
  payload["action"] = action;
  payload["handledLocally"] = handled_locally;
  payload["delta"] = value;
  if (binding_value && *binding_value) payload["value"] = binding_value;
  payload["tsMs"] = millis();
  protocolTransport().sendDocument(doc);
}

void BoardRuntime::pollKeys() {
  const uint32_t now = millis();
  for (size_t i = 0; i < 5; ++i) {
    const bool pressed = digitalRead(kKeys[i]) == LOW;
    const bool previous = (key_state_ & (1U << i)) == 0;
    if (pressed == previous) {
      if (pressed && key_pressed_at_[i] != 0 && !key_hold_sent_[i] && now - key_pressed_at_[i] >= kHoldMs) {
        char hold_event[32];
        snprintf(hold_event, sizeof(hold_event), "button.sw%u.hold", static_cast<unsigned>(i + 1));
        if (strcmp(input_config_.actionFor(hold_event), "disabled") != 0) {
          emitInput(hold_event, "hold_start");
          key_hold_sent_[i] = true;
        }
      }
      continue;
    }
    if (now - key_changed_at_[i] < kDebounceMs) continue;
    key_changed_at_[i] = now;
    if (pressed) {
      key_state_ &= static_cast<uint8_t>(~(1U << i));
      key_pressed_at_[i] = now;
      key_hold_sent_[i] = false;
    } else {
      key_state_ |= static_cast<uint8_t>(1U << i);
      char event[32];
      if (key_hold_sent_[i]) {
        snprintf(event, sizeof(event), "button.sw%u.hold", static_cast<unsigned>(i + 1));
        emitInput(event, "hold_end");
      } else if (key_pressed_at_[i] != 0 && now - key_pressed_at_[i] >= kHoldMs) {
        snprintf(event, sizeof(event), "button.sw%u.long_press", static_cast<unsigned>(i + 1));
        emitInput(event, "long_press");
      } else {
        snprintf(event, sizeof(event), "button.sw%u.short_press", static_cast<unsigned>(i + 1));
        emitInput(event, "short_press");
      }
      key_pressed_at_[i] = 0;
      key_hold_sent_[i] = false;
    }
  }
}

void BoardRuntime::poll() {
  pollKeys();
  if (avatar_.poll()) drawAvatar();
  if (voice_capture_active_ && millis() - last_voice_capture_frame_at_ >= 70)
    drawVoiceCaptureFrame(false);
  pollEdgeLight();
}

void BoardRuntime::configureEdgeLight(const String& state) {
  edge_light_state_ = state;
  edge_light_frame_ = 0;
  last_edge_light_frame_at_ = 0;
  edge_light_complete_ = false;
  edge_light_energy_rail_ = (esp_random() & 1U) != 0;
  edge_light_color_ = kMuted;
  edge_light_active_ = true;
  if (state == "WORKING" || state == "SPEAKING") edge_light_color_ = kCyan;
  else if (state == "WAITING") edge_light_color_ = kAmber;
  else if (state == "DONE") edge_light_color_ = kGreen;
  else if (state == "ERROR") edge_light_color_ = kRed;
  else edge_light_active_ = false;
}

void BoardRuntime::pollEdgeLight() {
  if (!display_ready_ || modal_active_ || !edge_light_active_ || edge_light_complete_) return;
  const uint32_t now = millis();
  if (now - last_edge_light_frame_at_ < kEdgeLightFrameMs) return;
  drawEdgeLightFrame();
}

void BoardRuntime::drawEdgeLightFrame(bool force) {
  if (!display_ready_ || modal_active_ || !edge_light_active_ || edge_light_target_mask_ == 0) return;
  if (edge_light_complete_ && !force) return;
  for (uint8_t index = 1; index < 5; ++index) {
    if (edge_light_target_mask_ & (1U << index)) drawEdgeLightOnPanel(displays_[index]);
  }
  last_edge_light_frame_at_ = millis();
  if (!edge_light_complete_) ++edge_light_frame_;
}

void BoardRuntime::drawEdgeLightOnPanel(VoiceDisplay& panel) {
  if (!panel.initialized()) return;
  const bool done = edge_light_state_ == "DONE";
  const uint32_t speed = edge_light_state_ == "WAITING" ? 2U
                         : edge_light_state_ == "ERROR" ? 6U
                         : edge_light_state_ == "SPEAKING" ? 5U
                         : edge_light_state_ == "DONE" ? 7U : 4U;
  const uint32_t cycle = kEdgeHalfLength + 24U;
  const auto draw_trail = [&](uint32_t frame, bool erase) {
    const uint32_t frame_travel = frame * speed;
    const int frame_head = done
        ? min(static_cast<uint32_t>(kEdgeHalfLength), frame_travel)
        : static_cast<int>(frame_travel % cycle);
    if (frame_head > kEdgeHalfLength) return;
    const uint8_t samples = edge_light_energy_rail_ ? 25 : 16;
    const uint8_t stride = 2;
    for (uint8_t sample = 0; sample < samples; ++sample) {
      const int distance = frame_head - sample * stride;
      if (distance < 0) continue;
      uint8_t brightness = static_cast<uint8_t>(edge_light_energy_rail_
          ? max(48, 255 - sample * 8)
          : max(32, 255 - sample * 14));
      if (edge_light_state_ == "ERROR" && ((frame / 5U) & 1U))
        brightness = static_cast<uint8_t>(brightness / 2U);
      const uint16_t color = erase ? kBackground : scale565(edge_light_color_, brightness);
      for (uint8_t side = 0; side < 2; ++side) {
        int16_t x = 0;
        int16_t y = 0;
        edgePoint(distance, side != 0, x, y);
        panel.fillCircle(x, y, sample < 3 ? 2 : 1, color);
      }
    }
  };

  const uint32_t travel = edge_light_frame_ * speed;
  panel.startWrite();
  if (edge_light_frame_ > 0) draw_trail(edge_light_frame_ - 1, true);

  if (done && travel >= static_cast<uint32_t>(kEdgeHalfLength + 36)) {
    edge_light_complete_ = true;
    panel.endWrite();
    return;
  }

  draw_trail(edge_light_frame_, false);
  panel.endWrite();
}

void BoardRuntime::drawWrappedText(VoiceDisplay& panel, String text, int y,
                                   uint16_t color, uint8_t max_lines) {
  text.replace('\r', ' ');
  text.replace('\n', ' ');
  text.trim();
  panel.setTextSize(1);
  panel.setTextColor(color, kBackground);
  size_t cursor = 0;
  bool clipped = false;
  for (uint8_t line = 0; line < max_lines && cursor < text.length(); ++line) {
    String value;
    size_t accepted = cursor;
    while (cursor < text.length()) {
      size_t bytes = 1;
      const uint8_t lead = static_cast<uint8_t>(text[cursor]);
      if ((lead & 0xe0) == 0xc0) bytes = 2;
      else if ((lead & 0xf0) == 0xe0) bytes = 3;
      else if ((lead & 0xf8) == 0xf0) bytes = 4;
      if (cursor + bytes > text.length()) bytes = 1;
      const String candidate = value + text.substring(cursor, cursor + bytes);
      if (!value.isEmpty() && panel.textWidth(candidate) > kPanelTextWidth) break;
      value = candidate;
      cursor += bytes;
      accepted = cursor;
    }
    if (accepted == cursor && value.isEmpty()) {
      value = text.substring(cursor, cursor + 1);
      ++cursor;
    }
    while (cursor < text.length() && text[cursor] == ' ') ++cursor;
    if (line + 1 == max_lines && cursor < text.length()) {
      clipped = true;
      while (!value.isEmpty() && panel.textWidth(value + "...") > kPanelTextWidth)
        value.remove(value.length() - 1);
      value += "...";
      cursor = text.length();
    }
    panel.drawString(value, kPanelTextLeft, y + line * 17);
  }
  (void)clipped;
}

void BoardRuntime::drawPanel(VoiceDisplay& panel, uint16_t accent, const char* title,
                             const String& body, const String& footer) {
  if (!panel.initialized()) return;
  panel.fillScreen(kBackground);
  panel.fillRect(0, 0, 128, 3, accent);
  panel.fillRect(6, 22, 116, 1, kSurface);
  panel.setTextColor(accent, kBackground);
  panel.setTextSize(1);
  panel.drawString(title, kPanelTextLeft, 10);
  drawWrappedText(panel, body.isEmpty() ? "--" : body, 31, TFT_WHITE, footer.isEmpty() ? 5 : 4);
  if (!footer.isEmpty()) {
    panel.fillRect(6, 103, 116, 1, kSurface);
    panel.setTextColor(kMuted, kBackground);
    String bounded = footer;
    while (!bounded.isEmpty() && panel.textWidth(bounded) > kPanelTextWidth)
      bounded.remove(bounded.length() - 1);
    panel.drawString(bounded, kPanelTextLeft, 109);
  }
}

void BoardRuntime::drawApprovalKey(VoiceDisplay& panel, uint16_t accent,
                                   const char* action, uint8_t key) {
  drawActionKey(panel, accent, action, key, "PRESS NOW");
}

void BoardRuntime::drawActionKey(VoiceDisplay& panel, uint16_t accent,
                                 const char* action, uint8_t key,
                                 const char* prompt) {
  if (!panel.initialized()) return;
  panel.fillScreen(TFT_BLACK);
  // The physical lens masks the outer corner pixels. Keep the complete frame
  // inside that mask and use two adjacent strokes for a uniform 2 px weight.
  panel.drawRoundRect(6, 6, 116, 116, 14, accent);
  panel.drawRoundRect(7, 7, 114, 114, 13, accent);
  panel.fillRoundRect(44, 10, 40, 4, 2, accent);
  panel.setTextColor(accent, TFT_BLACK);
  panel.setTextSize(2);
  const int action_x = max(4, static_cast<int>((128 - panel.textWidth(action)) / 2));
  panel.drawString(action, action_x, 31);
  panel.setTextSize(1);
  const String key_label = String("KEY ") + static_cast<unsigned>(key);
  panel.drawString(key_label,
                   max(4, static_cast<int>((128 - panel.textWidth(key_label)) / 2)), 70);
  panel.setTextColor(TFT_WHITE, TFT_BLACK);
  const String prompt_text = prompt;
  panel.drawString(prompt_text,
                   max(4, static_cast<int>((128 - panel.textWidth(prompt_text)) / 2)), 96);
}

void BoardRuntime::drawBoundActionKeys() {
  const int confirm_key = shortKeyForAction("page_enter");
  const int cancel_key = shortKeyForAction("page_back");
  if (confirm_key > 0 && !(voice_capture_active_ && confirm_key == 3))
    drawActionKey(displays_[confirm_key - 1], kGreen, "SEND", confirm_key, "SHORT PRESS");
  if (cancel_key > 0 && !(voice_capture_active_ && cancel_key == 3))
    drawActionKey(displays_[cancel_key - 1], kRed, "CANCEL", cancel_key, "SHORT PRESS");
}

void BoardRuntime::drawVoiceCaptureFrame(bool full) {
  if (!display_ready_ || !voice_capture_active_) return;
  VoiceDisplay& panel = displays_[2];
  if (full) {
    panel.fillScreen(TFT_BLACK);
    panel.fillRect(0, 0, 128, 4, kGreen);
    panel.setTextColor(kGreen, TFT_BLACK);
    panel.setTextSize(2);
    panel.drawString("SPEAKING", 16, 14);
    panel.setTextSize(1);
    panel.setTextColor(kMuted, TFT_BLACK);
    panel.drawString("RELEASE TO SEND", 18, 111);
  }
  panel.fillRect(7, 43, 114, 57, TFT_BLACK);
  panel.drawFastHLine(8, 72, 112, kSurface);
  for (uint8_t bar = 0; bar < 11; ++bar) {
    const uint8_t wave = static_cast<uint8_t>((voice_capture_frame_ * 2 + bar * 5) & 31U);
    const uint8_t triangle = wave < 16 ? wave : 31 - wave;
    const uint8_t height = static_cast<uint8_t>(8 + triangle * 2);
    const int x = 10 + bar * 10;
    panel.fillRoundRect(x, 72 - height / 2, 5, height, 2,
                        bar % 3 == 1 ? kCyan : kGreen);
  }
  ++voice_capture_frame_;
  last_voice_capture_frame_at_ = millis();
}

void BoardRuntime::setVoiceCaptureActive(bool active) {
  voice_capture_active_ = active;
  if (!active) {
    if (shortKeyForAction("page_enter") != 3 && shortKeyForAction("page_back") != 3)
      edge_light_target_mask_ |= 1U << 2;
    return;
  }
  edge_light_target_mask_ &= static_cast<uint8_t>(~(1U << 2));
  voice_capture_frame_ = 0;
  last_voice_capture_frame_at_ = 0;
  drawVoiceCaptureFrame(true);
}

void BoardRuntime::drawAvatar() {
  if (!display_ready_ || !avatar_.active() || !avatar_.frame()) return;
  if (modal_active_) return;
  if (shortKeyForAction("page_enter") == 1 || shortKeyForAction("page_back") == 1) return;
  // Screen 1 always uses one uninterrupted full-frame transaction. Other
  // panels retain content-aware partial updates.
  displays_[0].drawFullFrame(avatar_.frame());
}

bool BoardRuntime::applyInputConfig(JsonObjectConst payload) {
  const bool applied = input_config_.apply(payload);
  if (applied) controls_dirty_ = true;
  return applied;
}

void BoardRuntime::resetInputConfig(const char* request_id) {
  input_config_.reset(request_id);
  controls_dirty_ = true;
}

bool BoardRuntime::selectNextAvatarState() {
  if (!avatar_.selectNextState()) return false;
  drawAvatar();
  system_dirty_ = true;
  return true;
}

bool BoardRuntime::handleAvatar(const char* topic, JsonObjectConst payload, JsonDocument& ack) {
  const bool handled = avatar_.handle(topic, payload, ack);
  if (handled && ack["ok"] == true &&
      (!strcmp(topic, "display/avatar_end") || !strcmp(topic, "display/avatar_clear"))) {
    system_dirty_ = true;
    if (avatar_.active()) drawAvatar();
  }
  return handled;
}

void BoardRuntime::renderStatus(const String& agent, const String& state,
                                const String& detail) {
  if (!display_ready_) return;
  modal_active_ = false;
  const bool first = !status_initialized_;
  const bool agent_changed = first || agent != last_agent_;
  const bool state_changed = first || state != last_state_;
  const bool detail_changed = first || detail != last_detail_;
  if (!agent_changed && !state_changed && !detail_changed && !controls_dirty_ && !system_dirty_) return;
  last_agent_ = agent;
  last_state_ = state;
  last_detail_ = detail;
  status_initialized_ = true;
  uint16_t state_color = kMuted;
  if (state == "WAITING") state_color = kAmber;
  else if (state == "DONE") state_color = kGreen;
  else if (state == "ERROR") state_color = kRed;
  else if (state == "WORKING" || state == "SPEAKING") state_color = kCyan;
  if (state_changed) configureEdgeLight(state);
  edge_light_target_mask_ = (1U << 2) | (1U << 4);
  const int confirm_key = shortKeyForAction("page_enter");
  const int cancel_key = shortKeyForAction("page_back");
  if (confirm_key == 2 || cancel_key == 2) edge_light_target_mask_ &= static_cast<uint8_t>(~(1U << 1));
  if (confirm_key == 3 || cancel_key == 3 || voice_capture_active_)
    edge_light_target_mask_ &= static_cast<uint8_t>(~(1U << 2));
  if (confirm_key == 5 || cancel_key == 5)
    edge_light_target_mask_ &= static_cast<uint8_t>(~(1U << 4));
  if (first || state_changed || agent_changed) {
    if (avatar_.active()) drawAvatar();
    else drawPanel(displays_[0], kCyan, "AGENT", agent, "NO AVATAR");
    drawPanel(displays_[4], state_color, state.c_str(), agent, "AGENT / TASK");
  }
  if (state_changed || detail_changed)
    drawPanel(displays_[2], state_color, "MESSAGE", detail, "LIVE STATUS");
  if (first || controls_dirty_) {
    String controls = String("OK  K") + (confirm_key > 0 ? String(confirm_key) : "-") +
                      "\nBACK K" + (cancel_key > 0 ? String(cancel_key) : "-");
    drawPanel(displays_[3], kCyan, "CONTROLS", controls, "K5 PET ACTION");
    controls_dirty_ = false;
  }
  system_dirty_ = false;
  drawBoundActionKeys();
  drawEdgeLightFrame(true);
}

void BoardRuntime::renderVoice(const String& title, const String& body) {
  drawPanel(displays_[2], kCyan, title.c_str(), body, "VOICE");
}

void BoardRuntime::renderSendResult(bool ok, const String& detail) {
  voice_capture_active_ = false;
  drawPanel(displays_[2], ok ? kGreen : kRed, ok ? "SEND OK" : "SEND FAILED",
            detail.isEmpty() ? (ok ? "VOICE UPLOADED" : "CHECK USB") : detail,
            ok ? "DELIVERED" : "NOT SENT");
}

void BoardRuntime::renderConversation(const String& state, const String& name,
                                      const String& text) {
  String normalized = state;
  normalized.toLowerCase();
  uint16_t accent = kCyan;
  const char* title = "CONVERSATION";
  String fallback = name;
  if (normalized == "listening") {
    title = "LISTENING";
    fallback = "SPEAK NOW";
  } else if (normalized == "thinking" || normalized == "preparing") {
    title = "THINKING";
    fallback = "PROCESSING";
  } else if (normalized == "speaking") {
    title = "SPEAKING";
    fallback = name.isEmpty() ? "REPLY" : name;
  } else if (normalized == "error") {
    title = "CHAT ERROR";
    accent = kRed;
    fallback = "PLEASE RETRY";
  } else if (normalized == "ended") {
    title = "CHAT ENDED";
    accent = kMuted;
    fallback = "READY";
  }
  drawPanel(displays_[2], accent, title, text.isEmpty() ? fallback : text,
            "REALTIME");
}

void BoardRuntime::renderApproval(const String& agent, const String& tool,
                                  const String& reason) {
  if (!display_ready_) return;
  modal_active_ = true;
  avatar_.selectState("approval");
  last_agent_ = "__approval__";
  last_state_ = "__approval__";
  last_detail_ = reason;
  const bool has_confirm = shortKeyForAction("page_enter") > 0;
  const bool has_cancel = shortKeyForAction("page_back") > 0;
  uint8_t context = 0;
  for (uint8_t index = 0; index < 5; ++index) {
    char event[32];
    snprintf(event, sizeof(event), "button.sw%u.short_press", static_cast<unsigned>(index + 1));
    const char* action = input_config_.actionFor(event);
    const bool allow = !strcmp(action, "page_enter") || (!has_confirm && index == 0);
    const bool deny = !strcmp(action, "page_back") || (!has_cancel && index == 3);
    if (allow) {
      drawApprovalKey(displays_[index], kGreen, "ALLOW", index + 1);
    } else if (deny) {
      drawApprovalKey(displays_[index], kRed, "DENY", index + 1);
    } else if (context == 0) {
      drawPanel(displays_[index], kAmber, "APPROVAL", agent, "ACTION REQUIRED");
      ++context;
    } else if (context == 1) {
      drawPanel(displays_[index], kCyan, "TOOL", tool.isEmpty() ? "ACTION" : tool, "REQUEST");
      ++context;
    } else {
      drawPanel(displays_[index], kAmber, "REASON", reason.isEmpty() ? "ALLOW?" : reason, "REVIEW");
      ++context;
    }
  }
  controls_dirty_ = true;
  system_dirty_ = true;
}

