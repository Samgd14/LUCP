#pragma once

#include "message.hpp"
#include "protocol.hpp"
#include "transport.hpp"
#include <cstring>

namespace lucp {

namespace internal {

/**
 * @brief Circular queue for buffering outgoing ACKs (Echoes).
 * Fully statically allocated, bounded by EchoQueueDepth.
 * Stores only the necessary 4-byte headers to echo back a receipt confirmation.
 */
template <size_t EchoQueueDepth> class EchoQueue {
public:
  /**
   * @brief Individual echo packet buffered for dispatch.
   */
  struct EchoRecord {
    uint32_t dest_ip;
    uint16_t dest_port;
    uint8_t header[HEADER_SIZE];
  };

  void reset() {
    m_head = 0;
    m_tail = 0;
    m_count = 0;
  }

  bool enqueue(const uint8_t *header, uint32_t dest_ip, uint16_t dest_port) {
    if (m_count >= EchoQueueDepth)
      return false;
    auto &echo = m_queue[m_tail];
    echo.dest_ip = dest_ip;
    echo.dest_port = dest_port;
    std::memcpy(echo.header, header, HEADER_SIZE);
    m_tail = (m_tail + 1) % EchoQueueDepth;
    m_count++;
    return true;
  }

  bool is_full() const { return m_count >= EchoQueueDepth; }

  void flush(ITransport &transport) {
    while (m_count > 0) {
      const auto &echo = m_queue[m_head];
      transport.send(echo.header, HEADER_SIZE, echo.dest_ip, echo.dest_port);
      m_head = (m_head + 1) % EchoQueueDepth;
      m_count--;
    }
  }

private:
  EchoRecord m_queue[EchoQueueDepth] = {};
  size_t m_head = 0;
  size_t m_tail = 0;
  size_t m_count = 0;
};

/**
 * @brief Manager for pending acknowledgments and retry logic.
 * Keeps a statically bounded tracking array of all messages
 * waiting for an ACK. Flushes retries automatically on tick().
 */
template <size_t MaxPendingAcks, size_t MaxPayloadSize> class AckManager {
public:
  /**
   * @brief A tracked message that is waiting for an ACK.
   */
  struct PendingAck {
    bool active;
    uint8_t msg_id;
    uint8_t seq_id;
    uint32_t dest_ip;
    uint16_t dest_port;
    uint8_t retries_remaining;
    uint16_t packet_len;
    uint32_t last_tx_ms;
    uint8_t packet[HEADER_SIZE + MaxPayloadSize];
  };

  void reset() {
    for (size_t i = 0; i < MaxPendingAcks; ++i) {
      m_pending[i].active = false;
    }
  }

  int allocate() {
    for (size_t i = 0; i < MaxPendingAcks; ++i) {
      if (!m_pending[i].active)
        return static_cast<int>(i);
    }
    return -1;
  }

  PendingAck *get(int index) {
    if (index < 0 || index >= static_cast<int>(MaxPendingAcks))
      return nullptr;
    return &m_pending[index];
  }

  void clear(uint8_t msg_id, uint8_t seq_id, uint32_t dest_ip,
             uint16_t dest_port) {
    for (size_t i = 0; i < MaxPendingAcks; ++i) {
      auto &pend = m_pending[i];
      if (pend.active && pend.msg_id == msg_id && pend.seq_id == seq_id &&
          pend.dest_ip == dest_ip && pend.dest_port == dest_port) {
        pend.active = false;
        break;
      }
    }
  }

  template <size_t MsgCount>
  void tick(ITransport &transport, uint32_t now_ms,
            IMessage *const (&registry)[MsgCount]) {
    for (size_t i = 0; i < MaxPendingAcks; ++i) {
      auto &pend = m_pending[i];
      if (!pend.active)
        continue;

      IMessage *item = registry[pend.msg_id];
      if (!item) {
        pend.active = false;
        continue;
      }

      // Handle unsigned 32-bit timestamp wraparound safely
      if ((now_ms - pend.last_tx_ms) >= item->retry_delay_ms()) {
        if (pend.retries_remaining > 0) {
          pend.retries_remaining--;
          pend.last_tx_ms = now_ms;
          transport.send(pend.packet, pend.packet_len, pend.dest_ip,
                         pend.dest_port);
        } else {
          // Exhausted all transmission attempts
          pend.active = false;
          item->on_fail();
        }
      }
    }
  }

private:
  PendingAck m_pending[MaxPendingAcks] = {};
};

} // namespace internal

/**
 * @brief The core LUCP Protocol Node.
 * Fully templated to ensure zero dynamic allocation.
 */
template <size_t MsgCount = 256, size_t EchoQueueDepth = 16,
          size_t MaxPendingAcks = 4, size_t MaxPayloadSize = 256>
class Node : public INode {
public:
  explicit Node(ITransport &transport) : m_transport(transport) { reset(); }

  void reset() {
    for (size_t i = 0; i < MsgCount; ++i) {
      m_registry[i] = nullptr;
      m_seq_id[i] = 0;
    }
    m_ack_manager.reset();
    m_echo_queue.reset();
  }

  /**
   * @brief Register a strong-typed message object.
   */
  int register_message(IMessage *msg) {
    if (!msg)
      return ERR_BAD_ARG;
    uint8_t msg_id = msg->id();
    if (msg_id == 0 || msg_id >= MsgCount)
      return ERR_INVALID_ID;
    if (msg->size() == 0 || msg->size() > MaxPayloadSize)
      return ERR_BAD_ARG;

    msg->set_node(this);
    m_registry[msg_id] = msg;
    return OK;
  }

  /**
   * @brief Dispatch raw bytes for a message ID (override from INode).
   * @warning This function allocates `HEADER_SIZE + MaxPayloadSize` bytes on
   * the stack to construct the packet before sending. On highly constrained
   * devices (e.g. RP2040), ensure `MaxPayloadSize` bounded to avoid stack
   * overflows.
   */
  int send_raw(uint8_t msg_id, const uint8_t *payload, uint16_t payload_size,
               uint32_t dest_ip, uint16_t dest_port) override {
    if (msg_id == 0 || msg_id >= MsgCount)
      return ERR_INVALID_ID;

    IMessage *item = m_registry[msg_id];
    if (!item)
      return ERR_INVALID_ID; // Unregistered

    // Validate payload size against registered size
    if (payload_size != item->size())
      return ERR_BAD_ARG;

    const size_t packet_len = HEADER_SIZE + item->size();
    if (item->size() > MaxPayloadSize)
      return ERR_BAD_ARG;

    // Assign the next rolling sequence ID (wraps at 256)
    uint8_t seq_id = m_seq_id[msg_id]++;

    // Build the packet directly on the runtime stack
    uint8_t packet[HEADER_SIZE + MaxPayloadSize];
    packet[0] = MAGIC_0;
    packet[1] = MAGIC_1;
    packet[2] = msg_id;
    packet[3] = seq_id;

    // Copy payload after the 4 byte header
    if (item->size() > 0) {
      if (payload == nullptr)
        return ERR_BAD_ARG;
      std::memcpy(packet + HEADER_SIZE, payload, item->size());
    }

    // If ACK required, add to ACK manager
    if (item->ack_required()) {
      int pending_idx = m_ack_manager.allocate();
      if (pending_idx < 0)
        return ERR_QUEUE_FULL;

      auto *pend = m_ack_manager.get(pending_idx);
      pend->msg_id = msg_id;
      pend->seq_id = seq_id;
      pend->dest_ip = dest_ip;
      pend->dest_port = dest_port;
      pend->retries_remaining = item->max_retries();
      pend->packet_len = static_cast<uint16_t>(packet_len);
      pend->last_tx_ms = m_transport.now_ms();
      std::memcpy(pend->packet, packet, packet_len);
      pend->active = true;
    }

    // Send packet via transport PAL
    int rc = m_transport.send(packet, static_cast<uint16_t>(packet_len),
                              dest_ip, dest_port);

    // If send fails, clear ACK manager
    if (rc < 0) {
      if (item->ack_required()) {
        m_ack_manager.clear(msg_id, seq_id, dest_ip, dest_port);
      }
      return rc;
    }
    return OK;
  }

  void process_packet(const uint8_t *packet, uint16_t size, uint32_t source_ip,
                      uint16_t source_port) {
    // Check magic bytes and minimum size
    if (!packet || size < HEADER_SIZE)
      return;
    if (packet[0] != MAGIC_0 || packet[1] != MAGIC_1)
      return;

    // Extract message ID and sequence ID
    uint8_t msg_id = packet[2];
    uint8_t seq_id = packet[3];

    // Check if message ID is valid
    if (msg_id == 0 || msg_id >= MsgCount) {
      m_transport.log_unknown(msg_id, source_ip, source_port);
      return;
    }

    // Get message handler
    IMessage *item = m_registry[msg_id];
    if (!item) {
      m_transport.log_unknown(msg_id, source_ip, source_port);
      return;
    }

    // Reject zero-sized messages
    if (item->size() == 0) {
      uint16_t received = size >= HEADER_SIZE ? size - HEADER_SIZE : 0;
      m_transport.log_rejected(msg_id, received, 1);
      return;
    }

    // Check if this is an ACK (header only)
    if (size == HEADER_SIZE) {
      if (!item->ack_required()) {
        m_transport.log_rejected(msg_id, 0, item->size());
        return;
      }
      m_ack_manager.clear(msg_id, seq_id, source_ip, source_port);
      return;
    }

    // Validate payload size
    uint16_t payload_size = size - HEADER_SIZE;
    if (payload_size != item->size()) {
      m_transport.log_rejected(msg_id, payload_size, item->size());
      return;
    }

    // If an ACK is required, drop it if we cannot echo
    if (item->ack_required() && m_echo_queue.is_full()) {
      return;
    }

    // Dispatch payload to the correct handler
    int rc = item->handle(packet + HEADER_SIZE, payload_size);
    if (rc == ERR_NOT_IMPLEMENTED) {
      m_transport.log_unknown(msg_id, source_ip, source_port);
      return;
    }

    // Add to echo queue if ACK is required
    if (item->ack_required()) {
      m_echo_queue.enqueue(packet, source_ip, source_port);
    }
  }

  /**
   * @brief Node heartbeat tick. Continually clears ACKs and pending loops.
   */
  void tick() {
    uint32_t now_ms = m_transport.now_ms();
    m_echo_queue.flush(m_transport);
    m_ack_manager.tick(m_transport, now_ms, m_registry);
  }

private:
  ITransport &m_transport;

  IMessage *m_registry[MsgCount] = {};
  uint8_t m_seq_id[MsgCount] = {};

  internal::EchoQueue<EchoQueueDepth> m_echo_queue;
  internal::AckManager<MaxPendingAcks, MaxPayloadSize> m_ack_manager;
};

} // namespace lucp
