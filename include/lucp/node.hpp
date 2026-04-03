#pragma once

#include "message.hpp"
#include "protocol.hpp"
#include "transport.hpp"
#include <cstring>

namespace lucp
{

  namespace internal
  {

    /**
     * @brief Circular queue for buffering outgoing ACKs (Echoes).
     * Fully statically allocated, bounded by EchoQueueDepth.
     * Stores only the necessary 4-byte headers to echo back a receipt confirmation.
     */
    template <size_t EchoQueueDepth>
    class EchoQueue

    {
    public:
      /**
       * @brief Individual echo packet buffered for dispatch.
       */
      struct EchoRecord
      {
        uint32_t dest_ip;
        uint16_t dest_port;
        uint8_t header[HEADER_SIZE];
      };

      /**
       * @brief Clears all buffered echo records.
       */
      void reset()
      {
        m_head = 0;
        m_tail = 0;
        m_count = 0;
      }

      /**
       * @brief Enqueue an echo record for dispatch.
       * @param packet The received packet to acknowledge.
       * @param dest_ip The destination IP address.
       * @param dest_port The destination port.
       * @return OK (0) on success, or ERR_QUEUE_FULL if the ACK queue is full.
       */
      int enqueue(const uint8_t *packet, uint32_t dest_ip, uint16_t dest_port)
      {
        // Check if the queue is full before enqueuing
        if (m_count >= EchoQueueDepth)
          return ERR_QUEUE_FULL;

        // Store the echo record in the circular queue
        auto &echo = m_queue[m_tail];
        echo.dest_ip = dest_ip;
        echo.dest_port = dest_port;
        std::memcpy(echo.header, packet, HEADER_SIZE);

        // Advance tail and increment count
        m_tail = (m_tail + 1) % EchoQueueDepth;
        m_count++;
        return OK;
      }

      /**
       * @brief Checks whether the queue can accept more echo records.
       * @return True when the queue is full.
       */
      bool is_full() const { return m_count >= EchoQueueDepth; }

      /**
       * @brief Sends all queued echo records through the transport.
       * @param transport Transport implementation used for packet transmission.
       */
      void flush(ITransport &transport)
      {
        while (m_count > 0)
        {
          const auto &echo = m_queue[m_head];
          int rc = transport.send(echo.header, HEADER_SIZE, echo.dest_ip, echo.dest_port);
          if (rc < 0)
            transport.log_error(rc, echo.dest_ip, echo.dest_port);
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
    template <size_t MaxPendingAcks, size_t MaxPayloadSize>
    class AckManager
    {
    public:
      /**
       * @brief A tracked message that is waiting for an ACK.
       */
      struct PendingAck
      {
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

      /**
       * @brief Clears all tracked pending acknowledgments.
       */
      void reset()
      {
        for (size_t i = 0; i < MaxPendingAcks; ++i)
        {
          m_pending[i].active = false;
        }
      }

      /**
       * @brief Gets the next inactive pending-ACK entry.
       * @return Pointer to the entry, or nullptr if no inactive entries.
       */
      PendingAck *get_next()
      {
        for (size_t i = 0; i < MaxPendingAcks; ++i)
        {
          if (!m_pending[i].active)
            return &m_pending[i];
        }
        return nullptr;
      }

      /**
       * @brief Clears the first pending entry matching message identity and peer.
       * @param msg_id Message ID.
       * @param seq_id Sequence ID.
       * @param dest_ip Destination IP tracked for the pending packet.
       * @param dest_port Destination port tracked for the pending packet.
       */
      void clear(uint8_t msg_id, uint8_t seq_id, uint32_t dest_ip,
                 uint16_t dest_port)
      {
        for (size_t i = 0; i < MaxPendingAcks; ++i)
        {
          auto &pend = m_pending[i];
          if (pend.active && pend.msg_id == msg_id && pend.seq_id == seq_id &&
              pend.dest_ip == dest_ip && pend.dest_port == dest_port)
          {
            pend.active = false;
            break;
          }
        }
      }

      /**
       * @brief Processes retransmissions and timeout failures for active entries.
       * @tparam MsgCount Size of the message registry.
       * @param transport Transport implementation used for retransmission.
       * @param now_ms Current monotonic tick in milliseconds.
       * @param registry Message registry indexed by msg_id.
       */
      template <size_t MsgCount>
      void tick(ITransport &transport, uint32_t now_ms, IMessage *const (&registry)[MsgCount])
      {
        for (size_t i = 0; i < MaxPendingAcks; ++i)
        {
          auto &pend = m_pending[i];
          if (!pend.active)
            continue;

          // Get the registered message for retry parameters
          IMessage *msg = registry[pend.msg_id];

          // Check if it's time to retry
          if ((now_ms - pend.last_tx_ms) >= msg->retry_delay_ms())
          {
            // Check if we have retries left
            if (pend.retries_remaining > 0)
            {
              pend.retries_remaining--;
              pend.last_tx_ms = now_ms;
              transport.send(pend.packet, pend.packet_len, pend.dest_ip, pend.dest_port);
            }
            else
            {
              // Exhausted all transmission attempts
              pend.active = false;
              int rc = msg->on_fail();
              if (rc < 0)
                transport.log_error(rc, pend.dest_ip, pend.dest_port);
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
            size_t MaxPendingAcks = 4, size_t MaxPayloadSize = 256,
            size_t MaxRecvBurst = 64>
  class Node : public INode
  {
  public:
    static_assert(MsgCount > 1, "MsgCount must be greater than 1 to allow registration of messages (ID 0 is reserved).");
    static_assert(MsgCount <= 256, "MsgCount cannot exceed 256 because msg_id is uint8_t.");
    static_assert(MaxPendingAcks > 0, "MaxPendingAcks must be greater than 0 to allow tracking of messages.");
    static_assert(MaxPendingAcks < 256, "MaxPendingAcks must be < 256 to avoid seq_id aliasing.");
    static_assert(MaxPayloadSize > 0, "MaxPayloadSize must be greater than 0 to allow messages with payloads.");
    static_assert(MaxPayloadSize <= 1024, "MaxPayloadSize must be reasonably bounded to avoid stack overflow in send_raw and receive_incoming.");
    static_assert(MaxRecvBurst > 0, "MaxRecvBurst must be greater than 0 to allow processing of incoming packets.");
    static_assert(EchoQueueDepth > 0, "EchoQueueDepth must be greater than 0 to allow ACK echoes.");

    explicit Node(ITransport &transport) : m_transport(transport) { reset(); }

    /**
     * @brief Resets registry state, sequence counters, ACK tracking, and echo queue.
     */
    void reset()
    {
      for (size_t i = 0; i < MsgCount; ++i)
      {
        m_registry[i] = nullptr;
        m_seq_id[i] = 0;
      }
      m_ack_manager.reset();
      m_echo_queue.reset();
    }

    /**
     * @brief Resets only sequence counters, ACK tracking, and echo queue, preserving the message registry.
     */
    void reset_state()
    {
      for (size_t i = 0; i < MsgCount; ++i)
      {
        m_seq_id[i] = 0;
      }
      m_ack_manager.reset();
      m_echo_queue.reset();
    }

    /**
     * @brief Register a strong-typed message object.
     */
    int register_message(IMessage *msg)
    {
      // Reject null pointers
      if (!msg)
        return ERR_BAD_ARG;

      // Validate msg_id is within range (0 is reserved)
      uint8_t msg_id = msg->id();
      if (msg_id == 0 || msg_id >= MsgCount)
        return ERR_INVALID_ID;

      // Validate msg_size is within bounds
      uint16_t msg_size = msg->size();
      if (msg_size == 0 || msg_size > MaxPayloadSize)
        return ERR_INVALID_SIZE;

      // Register the message
      msg->set_node(this);
      m_registry[msg_id] = msg;
      return OK;
    }

    /**
     * @brief Get a registered message by its ID.
     * @param msg_id The message ID to retrieve.
     * @return Pointer to the registered message, or nullptr if not found.
     */
    IMessage *get_registered_message(uint8_t msg_id) const
    {
      // Validate msg_id is within valid range (0 is reserved)
      if (msg_id == 0 || msg_id >= MsgCount)
        return nullptr;

      // Check if message is actually registered in the registry
      IMessage *msg = m_registry[msg_id];
      if (!msg)
        return nullptr;

      return msg;
    }

    /**
     * @brief Dispatch raw bytes for a message ID (override from INode).
     * @warning This function allocates `HEADER_SIZE + MaxPayloadSize` bytes on
     * the stack to construct the packet before sending. On highly constrained
     * devices (e.g. RP2040), ensure `MaxPayloadSize` bounded to avoid stack
     * overflows.
     */
    int send_raw(uint8_t msg_id, const uint8_t *payload, uint16_t payload_size,
                 uint32_t dest_ip, uint16_t dest_port) override
    {
      // Get registered message instance for this ID
      IMessage *msg = get_registered_message(msg_id);
      if (!msg)
        return ERR_INVALID_ID;

      const uint16_t expected_size = msg->size();

      // Reject null payload
      if (expected_size > 0 && payload == nullptr)
        return ERR_BAD_ARG;

      // Validate payload size against registered size
      if (payload_size != expected_size)
        return ERR_INVALID_SIZE;

      const bool requires_ack = msg->ack_required();
      const uint16_t packet_len = static_cast<uint16_t>(HEADER_SIZE + expected_size);

      // Reserve an ACK slot if required
      typename internal::AckManager<MaxPendingAcks, MaxPayloadSize>::PendingAck *pend = nullptr;
      if (requires_ack)
      {
        pend = m_ack_manager.get_next();
        if (!pend)
          return ERR_QUEUE_FULL;
      }

      // For ack-required messages, build directly into the pending slot's buffer
      // For fire-and-forget messages, use a local stack buffer.
      uint8_t stack_packet[HEADER_SIZE + MaxPayloadSize];
      uint8_t *packet = requires_ack ? pend->packet : stack_packet;

      // Get the sequence ID
      uint8_t seq_id = m_seq_id[msg_id];

      // Build the packet
      packet[0] = MAGIC_0;
      packet[1] = MAGIC_1;
      packet[2] = msg_id;
      packet[3] = seq_id;

      // Copy payload after the 4 byte header
      std::memcpy(packet + HEADER_SIZE, payload, expected_size);

      // Send packet via transport PAL
      int rc = m_transport.send(packet, packet_len, dest_ip, dest_port);
      if (rc < 0)
        return rc;
      if (rc != static_cast<int>(packet_len))
        return ERR_PAL_SEND;

      // If send succeeds, increment sequence ID
      m_seq_id[msg_id]++;

      // If send succeeds, add to ACK tracking if required
      if (requires_ack)
      {
        pend->msg_id = msg_id;
        pend->seq_id = seq_id;
        pend->dest_ip = dest_ip;
        pend->dest_port = dest_port;
        pend->retries_remaining = msg->max_retries();
        pend->packet_len = packet_len;
        pend->last_tx_ms = m_transport.now_ms();
        pend->active = true;
      }

      return OK;
    }

    /**
     * @brief Validates and dispatches a single received packet.
     * @note Node is not thread-safe. It should only be called from a single thread.
     *
     * Header-only packets are treated as ACK confirmations for reliable
     * messages. Payload packets are dispatched to the registered message
     * handler and may enqueue an ACK echo when reliability is enabled.
     *
     * @return OK (0) on successful handling, or an error code.
     *
     * @param packet Packet bytes from the transport.
     * @param size Packet length in bytes.
     * @param source_ip Source IP address reported by the transport.
     * @param source_port Source port reported by the transport.
     */
    int process_packet(const uint8_t *packet, uint16_t size, uint32_t source_ip,
                       uint16_t source_port)
    {
      // Check for null pointer and minimum size
      if (!packet || size < HEADER_SIZE)
        return ERR_BAD_ARG;

      // Check magic bytes
      if (packet[0] != MAGIC_0 || packet[1] != MAGIC_1)
        return ERR_INVALID_PACKET;

      // Check for packet size exceeding maximum allowed
      if (size > (HEADER_SIZE + MaxPayloadSize))
        return ERR_PACKET_TOO_LARGE;

      // Extract message ID and sequence ID
      uint8_t msg_id = packet[2];
      uint8_t seq_id = packet[3];

      // Get message handler
      IMessage *msg = get_registered_message(msg_id);
      if (!msg)
      {
        return ERR_INVALID_ID;
      }

      const uint16_t expected_size = msg->size();
      const bool requires_ack = msg->ack_required();

      // Check if this is an ACK (header only)
      if (size == HEADER_SIZE)
      {
        if (!requires_ack)
        {
          return ERR_INVALID_PACKET;
        }
        m_ack_manager.clear(msg_id, seq_id, source_ip, source_port);
        return OK;
      }

      // Validate payload size
      uint16_t payload_size = size - HEADER_SIZE;
      if (payload_size != expected_size)
      {
        return ERR_INVALID_PACKET;
      }

      // If ACK is required but echo queue is full, drop the packet immediately
      if (requires_ack && m_echo_queue.is_full())
      {
        return ERR_QUEUE_FULL;
      }

      // Dispatch payload to the correct handler
      int rc = msg->handle(packet + HEADER_SIZE, payload_size);
      if (rc != OK)
        return rc;

      // Enqueue an ACK echo if required
      if (requires_ack)
        rc = m_echo_queue.enqueue(packet, source_ip, source_port);

      return rc;
    }

    /**
     * @brief Polls the transport for all available incoming packets and processes
     * them. This method uses a stack buffer of size `HEADER_SIZE +
     * MaxPayloadSize` bytes; ensure `MaxPayloadSize` is bounded appropriately for
     * embedded targets.
     *
     * Drains at most `MaxRecvBurst` packets per call to prevent starvation in a
     * main loop if the transport continuously reports data. Enqueues outgoing ACK
     * echoes but does not flush them - call `flush_echo_queue()` or
     * `process_all()` to dispatch them.
     *
     * The transport's `receive()` implementation MUST populate `src_ip` and
     * `src_port` whenever it returns a positive byte count.
     */
    void receive_incoming()
    {
      uint8_t buffer[HEADER_SIZE + MaxPayloadSize];
      uint32_t src_ip = 0;
      uint16_t src_port = 0;

      for (size_t i = 0; i < MaxRecvBurst; ++i)
      {
        int len = m_transport.receive(buffer, sizeof(buffer), src_ip, src_port);
        if (len <= 0)
          break;

        int rc = process_packet(buffer, static_cast<uint16_t>(len), src_ip, src_port);
        if (rc != OK)
          m_transport.log_error(rc, src_ip, src_port);
      }
    }

    /**
     * @brief Processes pending ACK retries and timeout failures.
     * Iterates all tracked outbound messages and retransmits those whose retry
     * delay has elapsed. Calls `IMessage::on_fail()` when retries are exhausted.
     * Should be called on a regular timer (e.g. every 10 ms) or in a main loop.
     */
    void ack_tick()
    {
      uint32_t now_ms = m_transport.now_ms();
      m_ack_manager.tick(m_transport, now_ms, m_registry);
    }

    /**
     * @brief Flushes all queued outgoing ACK echo packets via the transport.
     * Echoes are enqueued by `receive_incoming()` when a reliable message is
     * received. This must be called to actually dispatch them. May be called
     * independently or via `process_all()`.
     */
    void flush_echo_queue() { m_echo_queue.flush(m_transport); }

    /**
     * @brief Convenience method: polls for incoming packets, retries pending
     * ACKs, and flushes outgoing echo queue in one call. Suitable for simple
     * main-loop polling where all three operations share the same cadence.
     *
     * For interrupt-driven or RTOS designs, prefer calling `receive_incoming()`,
     * `ack_tick()`, and `flush_echo_queue()` independently at their respective
     * rates.
     */
    void process_all()
    {
      receive_incoming();
      ack_tick();
      flush_echo_queue();
    }

  private:
    ITransport &m_transport;

    IMessage *m_registry[MsgCount] = {};
    uint8_t m_seq_id[MsgCount] = {};

    internal::EchoQueue<EchoQueueDepth> m_echo_queue;
    internal::AckManager<MaxPendingAcks, MaxPayloadSize> m_ack_manager;
  };

} // namespace lucp
