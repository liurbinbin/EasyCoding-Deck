#include "protocol_transport.h"

#include <algorithm>
#include <utility>

namespace {
ProtocolTransport transport_instance;
}

ProtocolTransport& protocolTransport() {
  return transport_instance;
}

void ProtocolTransport::begin(TransportAdapter& usb, TransportAdapter& wifi,
                              OwnerChangedHandler owner_changed) {
  usb_ = &usb;
  wifi_ = &wifi;
  owner_changed_ = owner_changed;
  usb_->begin(*this);
  wifi_->begin(*this);

  TransportContext initial;
  initial.kind = TransportKind::Usb;
  initial.peer_id = 1;
  initial.authenticated = true;
  initial.valid = true;
  owner_ = claimOwner(initial);
}

TransportAdapter* ProtocolTransport::adapterFor(TransportKind kind) const {
  return kind == TransportKind::Wifi ? wifi_ : usb_;
}

TransportContext ProtocolTransport::contextFor(TransportKind kind,
                                               uint32_t peer_id,
                                               bool authenticated) const {
  TransportContext context;
  context.kind = kind;
  context.peer_id = peer_id;
  context.authenticated = authenticated;
  context.valid = peer_id != 0;
  if (owner_.valid && owner_.kind == kind && owner_.peer_id == peer_id) {
    context.generation = owner_.generation;
    context.authenticated = owner_.authenticated;
  }
  return context;
}

TransportContext ProtocolTransport::claimOwner(const TransportContext& candidate,
                                               bool force_new_generation) {
  if (!candidate.valid || !candidate.authenticated || candidate.peer_id == 0) return {};
  if (!force_new_generation && owner_.samePeer(candidate) && owner_.authenticated)
    return owner_;

  const TransportContext previous = owner_;
  if (previous.valid) clearOutboundFor(previous);
  TransportContext current = candidate;
  current.generation = next_generation_++;
  if (next_generation_ == 0) next_generation_ = 1;
  owner_ = current;

  if (owner_changed_) owner_changed_(previous, current);
  if (previous.valid && previous.kind == TransportKind::Wifi &&
      !previous.samePeer(current)) {
    if (TransportAdapter* adapter = adapterFor(previous.kind))
      adapter->closePeer(previous.peer_id);
  }
  return current;
}

void ProtocolTransport::releaseOwner(const TransportContext& connection) {
  if (!owner_.sameConnection(connection) && !owner_.samePeer(connection)) return;
  const TransportContext previous = owner_;
  clearOutboundFor(previous);
  owner_ = {};
  if (owner_changed_) owner_changed_(previous, owner_);
}

bool ProtocolTransport::admitted(const TransportContext& source) const {
  return source.authenticated && source.sameConnection(owner_);
}

bool ProtocolTransport::enqueueInbound(const TransportContext& source,
                                       const String& message) {
  if (!source.valid || message.isEmpty() || message.length() > kMaxMessageBytes) {
    ++dropped_inbound_;
    return false;
  }
  if (inbound_count_ >= kMaxInboundMessages) {
    ++dropped_inbound_;
    return false;
  }
  const size_t tail = (inbound_head_ + inbound_count_) % kMaxInboundMessages;
  inbound_[tail].context = source;
  inbound_[tail].message = message;
  ++inbound_count_;
  inbound_high_water_ = std::max<uint8_t>(inbound_high_water_, inbound_count_);
  return true;
}

bool ProtocolTransport::nextInbound(TransportContext& source, String& message) {
  if (inbound_count_ == 0) return false;
  MessageSlot& slot = inbound_[inbound_head_];
  source = slot.context;
  message = std::move(slot.message);
  slot.message = "";
  slot.context = {};
  inbound_head_ = (inbound_head_ + 1) % kMaxInboundMessages;
  --inbound_count_;
  return true;
}

bool ProtocolTransport::isBulkTopic(const char* topic) {
  if (!topic) return false;
  return !strcmp(topic, "audio/chunk") || !strcmp(topic, "system/heartbeat") ||
         !strcmp(topic, "power/status") || !strcmp(topic, "audio/status");
}

bool ProtocolTransport::enqueueOutbound(const TransportContext& target,
                                        const String& message, bool bulk_message) {
  if (!target.valid || message.isEmpty() || message.length() > kMaxMessageBytes) {
    ++dropped_outbound_;
    return false;
  }
  MessageSlot* queue = bulk_message ? bulk_ : control_;
  const size_t capacity = bulk_message ? kBulkQueueDepth : kControlQueueDepth;
  size_t& head = bulk_message ? bulk_head_ : control_head_;
  size_t& count = bulk_message ? bulk_count_ : control_count_;
  if (count >= capacity) {
    ++dropped_outbound_;
    return false;
  }
  const size_t tail = (head + count) % capacity;
  queue[tail].context = target;
  queue[tail].message = message;
  ++count;
  outbound_high_water_ = std::max<uint8_t>(
      outbound_high_water_, static_cast<uint8_t>(control_count_ + bulk_count_));
  return true;
}

bool ProtocolTransport::dequeueOutbound(MessageSlot* queue, size_t capacity,
                                        size_t& head, size_t& count,
                                        MessageSlot& result) {
  if (count == 0) return false;
  result.context = queue[head].context;
  result.message = std::move(queue[head].message);
  queue[head].context = {};
  queue[head].message = "";
  head = (head + 1) % capacity;
  --count;
  return true;
}

void ProtocolTransport::drainQueue(MessageSlot* queue, size_t capacity,
                                   size_t& head, size_t& count, size_t budget) {
  while (budget-- > 0 && count > 0) {
    MessageSlot& pending = queue[head];
    TransportAdapter* adapter = adapterFor(pending.context.kind);
    const TransportSendResult result = adapter
        ? adapter->sendText(pending.context, pending.message)
        : TransportSendResult::Failed;
    if (result == TransportSendResult::Busy) return;

    MessageSlot message;
    if (!dequeueOutbound(queue, capacity, head, count, message)) return;
    if (result == TransportSendResult::Failed) {
      ++dropped_outbound_;
      if (adapter) adapter->closePeer(message.context.peer_id);
      if (message.context.sameConnection(owner_)) {
        releaseOwner(message.context);
        return;
      }
    }
  }
}

void ProtocolTransport::compactQueue(MessageSlot* queue, size_t capacity,
                                     size_t& head, size_t& count,
                                     const TransportContext& retired) {
  MessageSlot retained[kBulkQueueDepth];
  size_t retained_count = 0;
  const size_t original_count = count;
  for (size_t offset = 0; offset < original_count; ++offset) {
    const size_t index = (head + offset) % capacity;
    MessageSlot slot;
    slot.context = queue[index].context;
    slot.message = std::move(queue[index].message);
    queue[index].context = {};
    queue[index].message = "";
    if (slot.context.sameConnection(retired)) {
      ++dropped_outbound_;
    } else {
      retained[retained_count++] = std::move(slot);
    }
  }
  for (size_t index = 0; index < retained_count; ++index)
    queue[index] = std::move(retained[index]);
  head = 0;
  count = retained_count;
}

void ProtocolTransport::clearOutboundFor(const TransportContext& retired) {
  if (!retired.valid || retired.generation == 0) return;
  compactQueue(control_, kControlQueueDepth, control_head_, control_count_, retired);
  compactQueue(bulk_, kBulkQueueDepth, bulk_head_, bulk_count_, retired);
}

bool ProtocolTransport::serializeAndQueue(const TransportContext& target,
                                          JsonDocument& document) {
  if (!target.valid || measureJson(document) > kMaxMessageBytes) {
    ++dropped_outbound_;
    return false;
  }
  const char* topic = document["topic"] | "";
  String serialized;
  serialized.reserve(measureJson(document) + 1);
  if (serializeJson(document, serialized) == 0) {
    ++dropped_outbound_;
    return false;
  }
  return enqueueOutbound(target, serialized, isBulkTopic(topic));
}

bool ProtocolTransport::send(const char* topic, JsonObjectConst payload) {
  const TransportContext target = dispatch_active_ ? dispatch_context_ : owner_;
  return sendTo(target, topic, payload);
}

bool ProtocolTransport::sendDocument(JsonDocument& document) {
  const TransportContext target = dispatch_active_ ? dispatch_context_ : owner_;
  return sendDocumentTo(target, document);
}

bool ProtocolTransport::sendTo(const TransportContext& target, const char* topic,
                               JsonObjectConst payload) {
  JsonDocument document;
  document["topic"] = topic;
  document["payload"] = payload;
  return serializeAndQueue(target, document);
}

bool ProtocolTransport::sendDocumentTo(const TransportContext& target,
                                       JsonDocument& document) {
  return serializeAndQueue(target, document);
}

void ProtocolTransport::poll() {
  if (usb_) usb_->poll();
  if (wifi_) wifi_->poll();
  drainQueue(control_, kControlQueueDepth, control_head_, control_count_, 4);
  drainQueue(bulk_, kBulkQueueDepth, bulk_head_, bulk_count_, 4);
}

void ProtocolTransport::flush(uint32_t timeout_ms) {
  const uint32_t started = millis();
  while ((control_count_ > 0 || bulk_count_ > 0) && millis() - started < timeout_ms) {
    drainQueue(control_, kControlQueueDepth, control_head_, control_count_, 8);
    drainQueue(bulk_, kBulkQueueDepth, bulk_head_, bulk_count_, 8);
    delay(1);
  }
  if (usb_) usb_->flush();
  if (wifi_) wifi_->flush();
}
