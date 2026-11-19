#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <cstdint>

// 传输来源由本地适配器注入，远端 JSON 无权声明这些字段。
enum class TransportKind : uint8_t {
  Usb = 0,
  Wifi = 1,
};

struct TransportContext {
  TransportKind kind = TransportKind::Usb;
  uint32_t generation = 0;
  uint32_t peer_id = 0;
  bool authenticated = false;
  bool valid = false;

  bool samePeer(const TransportContext& other) const {
    return valid && other.valid && kind == other.kind && peer_id == other.peer_id;
  }

  bool sameConnection(const TransportContext& other) const {
    return samePeer(other) && generation != 0 && generation == other.generation;
  }
};

class ProtocolTransport;

enum class TransportSendResult : uint8_t {
  Sent,
  Busy,
  Failed,
};

class TransportAdapter {
 public:
  virtual ~TransportAdapter() = default;
  virtual TransportKind kind() const = 0;
  virtual void begin(ProtocolTransport& transport) = 0;
  virtual void poll() = 0;
  virtual TransportSendResult sendText(const TransportContext& target,
                                       const String& message) = 0;
  virtual void closePeer(uint32_t peer_id) = 0;
  virtual void flush() = 0;
};

class ProtocolTransport {
 public:
  static constexpr size_t kMaxMessageBytes = 12288;
  static constexpr size_t kMaxInboundMessages = 32;
  static constexpr size_t kControlQueueDepth = 16;
  // Wi-Fi audio capture produces one frame every 20 ms; retain a bounded
  // jitter window instead of failing the whole conversation on one burst.
  static constexpr size_t kBulkQueueDepth = 32;

  using OwnerChangedHandler = void (*)(const TransportContext& previous,
                                       const TransportContext& current);

  void begin(TransportAdapter& usb, TransportAdapter& wifi, OwnerChangedHandler owner_changed);
  void poll();
  void flush(uint32_t timeout_ms = 250);

  bool enqueueInbound(const TransportContext& source, const String& message);
  bool inboundAvailable() const { return inbound_count_ < kMaxInboundMessages; }
  bool nextInbound(TransportContext& source, String& message);

  bool send(const char* topic, JsonObjectConst payload);
  bool sendDocument(JsonDocument& document);
  bool sendTo(const TransportContext& target, const char* topic, JsonObjectConst payload);
  bool sendDocumentTo(const TransportContext& target, JsonDocument& document);

  TransportContext claimOwner(const TransportContext& candidate,
                              bool force_new_generation = false);
  void releaseOwner(const TransportContext& connection);
  bool admitted(const TransportContext& source) const;
  TransportContext owner() const { return owner_; }
  TransportContext contextFor(TransportKind kind, uint32_t peer_id,
                              bool authenticated) const;

  void beginDispatch(const TransportContext& source) {
    dispatch_context_ = source;
    dispatch_active_ = source.valid;
  }
  void endDispatch() {
    dispatch_active_ = false;
    dispatch_context_ = {};
  }

  uint32_t droppedInbound() const { return dropped_inbound_; }
  uint32_t droppedOutbound() const { return dropped_outbound_; }
  uint8_t inboundHighWater() const { return inbound_high_water_; }
  uint8_t outboundHighWater() const { return outbound_high_water_; }

 private:
  struct MessageSlot {
    TransportContext context;
    String message;
  };

  bool enqueueOutbound(const TransportContext& target, const String& message,
                       bool bulk);
  bool dequeueOutbound(MessageSlot* queue, size_t capacity, size_t& head,
                       size_t& count, MessageSlot& result);
  void drainQueue(MessageSlot* queue, size_t capacity, size_t& head,
                  size_t& count, size_t budget);
  void compactQueue(MessageSlot* queue, size_t capacity, size_t& head,
                    size_t& count, const TransportContext& retired);
  void clearOutboundFor(const TransportContext& retired);
  TransportAdapter* adapterFor(TransportKind kind) const;
  bool serializeAndQueue(const TransportContext& target, JsonDocument& document);
  static bool isBulkTopic(const char* topic);

  TransportAdapter* usb_ = nullptr;
  TransportAdapter* wifi_ = nullptr;
  OwnerChangedHandler owner_changed_ = nullptr;
  TransportContext owner_;
  TransportContext dispatch_context_;
  bool dispatch_active_ = false;
  uint32_t next_generation_ = 1;

  MessageSlot inbound_[kMaxInboundMessages];
  size_t inbound_head_ = 0;
  size_t inbound_count_ = 0;
  MessageSlot control_[kControlQueueDepth];
  size_t control_head_ = 0;
  size_t control_count_ = 0;
  MessageSlot bulk_[kBulkQueueDepth];
  size_t bulk_head_ = 0;
  size_t bulk_count_ = 0;

  uint32_t dropped_inbound_ = 0;
  uint32_t dropped_outbound_ = 0;
  uint8_t inbound_high_water_ = 0;
  uint8_t outbound_high_water_ = 0;
};

ProtocolTransport& protocolTransport();
