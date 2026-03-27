#pragma once

#include <cstdint>

namespace lucp {

/**
 * @brief Platform Abstraction Layer for network transport and time.
 * Implement this interface for your specific hardware (e.g., Arduino EthernetUDP, ESP-IDF sockets, Linux sockets).
 */
class ITransport {
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
    virtual int send(const uint8_t* buf, uint16_t len, uint32_t ip, uint16_t port) = 0;

    /**
     * @brief Gets the current monotonic time in milliseconds.
     * @return Milliseconds since system start.
     */
    virtual uint32_t now_ms() = 0;

    /**
     * @brief Optional: Log general debug messages.
     */
    virtual void log_debug(const char* /*fmt*/, ...) {}

    /**
     * @brief Optional: Log when an unregistered message ID is received.
     */
    virtual void log_unknown(uint8_t /*msg_id*/, uint32_t /*src_ip*/, uint16_t /*src_port*/) {}

    /**
     * @brief Optional: Log when a message is rejected due to size mismatch.
     */
    virtual void log_rejected(uint8_t /*msg_id*/, uint16_t /*received_size*/, uint16_t /*expected_size*/) {}
};

} // namespace lucp
