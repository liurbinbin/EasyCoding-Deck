#include "usb_cdc_transport.h"

#include <ArduinoJson.h>
#include <cstring>

namespace {
constexpr uint32_t kUsbPeerId = 1;
constexpr size_t kUsbReadBudget = 16 * 1024;
constexpr size_t kUsbRxBufferBytes = 32 * 1024;
}

void UsbCdcTransport::begin(ProtocolTransport& transport) {
  transport_ = &transport;
  // HWCDC ISR 队列满会丢字节；容纳完整音频窗口，不只是一条 JSON。
  Serial.setRxBufferSize(kUsbRxBufferBytes);
  Serial.setTxBufferSize(4096);
  Serial.begin(115200);
  line_.reserve(ProtocolTransport::kMaxMessageBytes);
}

TransportContext UsbCdcTransport::context() const {
  return transport_ ? transport_->contextFor(TransportKind::Usb, kUsbPeerId, true)
                    : TransportContext{};
}

void UsbCdcTransport::reportOversize(const TransportContext& source) {
  JsonDocument payload;
  payload["ok"] = false;
  payload["code"] = "message_too_large";
  payload["maxBytes"] = ProtocolTransport::kMaxMessageBytes;
  transport_->sendTo(source, "protocol/error", payload.as<JsonObjectConst>());
}

void UsbCdcTransport::poll() {
  if (!transport_) return;
  size_t budget = kUsbReadBudget;
  while (budget-- > 0 && transport_->inboundAvailable() && Serial.available()) {
    const char value = static_cast<char>(Serial.read());
    if (value == '\n' || value == '\r') {
      if (discarding_oversize_) {
        reportOversize(context());
      } else if (!line_.isEmpty()) {
        if (tx_poisoned_) {
          JsonDocument probe;
          if (!deserializeJson(probe, line_) &&
              (!strcmp(probe["topic"] | "", "bind") ||
               !strcmp(probe["type"] | "", "hello")))
            tx_poisoned_ = false;
        }
        transport_->enqueueInbound(context(), line_);
      }
      line_ = "";
      discarding_oversize_ = false;
      continue;
    }
    if (discarding_oversize_) continue;
    if (line_.length() >= ProtocolTransport::kMaxMessageBytes) {
      line_ = "";
      discarding_oversize_ = true;
      continue;
    }
    line_ += value;
  }
}

TransportSendResult UsbCdcTransport::sendText(const TransportContext& target,
                                               const String& message) {
  const TransportContext active = context();
  if (!target.sameConnection(active) || tx_poisoned_)
    return TransportSendResult::Failed;
  String frame;
  frame.reserve(message.length() + 1);
  frame = message;
  frame += '\n';
  const size_t written = Serial.write(
      reinterpret_cast<const uint8_t*>(frame.c_str()), frame.length());
  if (written != frame.length()) {
    tx_poisoned_ = true;
    return TransportSendResult::Failed;
  }
  return TransportSendResult::Sent;
}

void UsbCdcTransport::closePeer(uint32_t peer_id) {
  (void)peer_id;
}

void UsbCdcTransport::flush() {
  Serial.flush();
}
