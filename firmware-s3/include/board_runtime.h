#pragma once

#include <Arduino.h>
#include <LovyanGFX.hpp>

#include "input_config.h"
#include "avatar_service.h"
#include "voice_display.h"

class BoardRuntime {
 public:
  using InputHandler = bool (*)(const char* event, const char* action, const char* gesture);

  void begin();
  void poll();
  void renderStatus(const String& agent, const String& state, const String& detail);
  void renderVoice(const String& title, const String& body);
  void renderSendResult(bool ok, const String& detail = "");
  void setVoiceCaptureActive(bool active);
  void renderConversation(const String& state, const String& name, const String& text);
  void renderApproval(const String& agent, const String& tool, const String& reason);
  void setInputHandler(InputHandler handler) { input_handler_ = handler; }
  bool applyInputConfig(JsonObjectConst payload);
  void sendInputConfigState(JsonObjectConst request) const { input_config_.sendState(request); }
  void resetInputConfig(const char* request_id);
  bool displayReady() const { return display_ready_; }
  int shortKeyForAction(const char* action) const { return input_config_.shortKeyForAction(action); }
  int approvalDecisionForShortEvent(const char* event) const {
    return input_config_.approvalDecisionForShortEvent(event);
  }
  void selectAvatarState(const char* state) { if (avatar_.selectState(state)) drawAvatar(); }
  bool selectNextAvatarState();
  bool handleAvatar(const char* topic, JsonObjectConst payload, JsonDocument& ack);
  bool avatarTransferActive() const { return avatar_.transferActive(); }
  void abortAvatarTransfer() { avatar_.abortForTransportChange(); }

 private:
  void pollKeys();
  void emitInput(const char* event, const char* gesture, int value = 0);
  void drawPanel(VoiceDisplay& panel, uint16_t accent, const char* title,
                 const String& body, const String& footer = "");
  void drawApprovalKey(VoiceDisplay& panel, uint16_t accent, const char* action,
                       uint8_t key);
  void drawActionKey(VoiceDisplay& panel, uint16_t accent, const char* action,
                     uint8_t key, const char* prompt);
  void drawBoundActionKeys();
  void drawVoiceCaptureFrame(bool full);
  void drawWrappedText(VoiceDisplay& panel, String text, int y, uint16_t color,
                       uint8_t max_lines);
  void configureEdgeLight(const String& state);
  void pollEdgeLight();
  void drawEdgeLightFrame(bool force = false);
  void drawEdgeLightOnPanel(VoiceDisplay& panel);
  void drawAvatar();

  lgfx::Bus_SPI bus_;
  VoiceDisplay displays_[5];
  bool display_ready_ = false;
  uint8_t key_state_ = 0xff;
  uint32_t key_changed_at_[5] = {};
  uint32_t key_pressed_at_[5] = {};
  bool key_hold_sent_[5] = {};
  bool status_initialized_ = false;
  bool controls_dirty_ = true;
  bool system_dirty_ = true;
  bool modal_active_ = false;
  bool voice_capture_active_ = false;
  uint32_t voice_capture_frame_ = 0;
  uint32_t last_voice_capture_frame_at_ = 0;
  uint32_t edge_light_frame_ = 0;
  uint32_t last_edge_light_frame_at_ = 0;
  uint16_t edge_light_color_ = 0;
  uint8_t edge_light_target_mask_ = 0;
  bool edge_light_active_ = false;
  bool edge_light_energy_rail_ = false;
  bool edge_light_complete_ = false;
  String edge_light_state_;
  String last_agent_;
  String last_state_;
  String last_detail_;
  InputConfig input_config_;
  InputHandler input_handler_ = nullptr;
  AvatarService avatar_;
};

