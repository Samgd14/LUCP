#pragma once

#include <cstdint>

namespace lucp
{

  /**
   * @brief Platform Abstraction Layer for network transport and time.
   * Implement this interface for your specific hardware (e.g., Arduino EthernetUDP, ESP-IDF sockets, Linux sockets).
   */
  class ITransport
  {
  public:
    virtual ~ITransport() = default;

    /**
     * @brief Sends a UDP packet to the specified IP and port.
     * @param buf Pointer to the data buffer to send.
     * @param len Length of the data buffer in bytes.
     * @param ip Destination IPv4 address in network byte order.
     * @param port Destination port number in host byte order.
     * @return Number of bytes sent, or a negative error code (e.g. protocol::ERR_PAL_SEND).
     */
    virtual int send(const uint8_t *buf, uint16_t len, uint32_t ip, uint16_t port) = 0;

    /**
     * @brief Receives one incoming UDP packet, non-blocking.
     * @param buf Pointer to the buffer where received data should be stored.
     * @param max_len Maximum length of the buffer in bytes.
     * @param src_ip [out] Source IP address in network byte order. MUST be populated when return > 0.
     * @param src_port [out] Source port in host byte order. MUST be populated when return > 0.
     * @return Number of bytes received, 0 if no packet is available, or a negative error code.
     *
     * @note The default implementation returns 0 (no data). Interrupt-driven or callback-based
     *       transports that feed data directly via `Node::process_packet()` may leave this
     *       as the default and skip calling `Node::receive_incoming()` entirely.
     */
    virtual int receive(uint8_t *buf, uint16_t max_len, uint32_t &src_ip, uint16_t &src_port)
    {
      (void)buf;
      (void)max_len;
      (void)src_ip;
      (void)src_port;
      return 0;
    }

    /**
     * @brief Gets the current monotonic time in milliseconds.
     * @return Milliseconds since system start.
     */
    virtual uint32_t now_ms() = 0;

    /**
     * @brief Optional: Log LUCP protocol/runtime errors by error code.
     *
     * Should be called at a higher-level boundary (e.g. receive loop),
     * not deep into packet-validation.
     */
    virtual void log_error(int error_code, uint32_t src_ip, uint16_t src_port) {}
  };

} // namespace lucp
