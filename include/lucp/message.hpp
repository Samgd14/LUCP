#pragma once

#include "protocol.hpp"
#include <cstdint>

namespace lucp
{

  /**
   * @brief Base interface for LUCP Nodes to decouple them from the IMessage
   * definitions while being usable in node functions.
   */
  class INode
  {
  public:
    virtual ~INode() = default;

    /**
     * @brief Dispatch a raw payload for a registered message ID.
     */
    virtual int send_raw(uint8_t msg_id, const uint8_t *payload,
                         uint16_t payload_size, uint32_t dest_ip,
                         uint16_t dest_port) = 0;
  };

  /**
   * @brief Abstract Base Class for defining an LUCP protocol message.
   * Embeds protocol definitions, handler logic, and a sending interface.
   */
  class IMessage
  {
  protected:
    INode *m_node = nullptr;

  public:
    virtual ~IMessage() = default;

    /**
     * @brief Invoked natively by Node::register_message.
     */
    void set_node(INode *node) { m_node = node; }

    // -------------------------------------------------------------
    // Protocol Configuration (must be implemented by subclasses)
    // -------------------------------------------------------------
    virtual uint8_t id() const = 0;
    virtual uint16_t size() const = 0;
    virtual bool ack_required() const = 0;
    virtual uint8_t max_retries() const { return 0; }
    virtual uint16_t retry_delay_ms() const { return 0; }

    // -------------------------------------------------------------
    // Receiver Handlers
    // -------------------------------------------------------------
    /**
     * @brief Called when a valid packet is completely received.
     * @return OK (0) on success, or an error code.
     *         Returning an error prevents the node from sending an ACK.
     */
    virtual int handle(const uint8_t *payload, uint16_t size)
    {
      return ERR_NOT_IMPLEMENTED;
    }

    /**
     * @brief Called when a transmitted, ack-required packet exhausts all retries.
     * @return OK (0) on success, or an error code.
     */
    virtual int on_fail() { return ERR_NOT_IMPLEMENTED; }
  };

  /**
   * @brief Optional helper for shared message definitions.
   * Automatically resolves size() and provides a strongly-typed send() wrapper.
   */
  template <typename TPayload>
  class TypedMessage : public IMessage
  {
  public:
    uint16_t size() const override
    {
      return static_cast<uint16_t>(sizeof(TPayload));
    }

    /**
     * @brief Strongly-typed send wrapper for this specific payload struct.
     */
    int send(const TPayload &payload, uint32_t dest_ip, uint16_t dest_port)
    {
      if (!m_node)
        return ERR_BAD_ARG; // Not registered to any node
      return m_node->send_raw(id(), reinterpret_cast<const uint8_t *>(&payload), size(), dest_ip, dest_port);
    }
  };

} // namespace lucp
