#pragma once

#include <Arduino.h>

#include "protocol_transport.h"

class UsbCdcTransport final : public TransportAdapter {
 public:
  TransportKind kind() const override { return TransportKind::Usb; }
  void begin(ProtocolTransport& transport) override;
  void poll() override;
  TransportSendResult sendText(const TransportContext& target,
                               const String& message) override;
  void closePeer(uint32_t peer_id) override;
  void flush() override;

  TransportContext context() const;

 private:
  void reportOversize(const TransportContext& source);

  ProtocolTransport* transport_ = nullptr;
  String line_;
  bool discarding_oversize_ = false;
  bool tx_poisoned_ = false;
};
