#pragma once

#include <iostream>

#define ASSERT_TRUE(condition)                                                                                 \
  do                                                                                                           \
  {                                                                                                            \
    if (!(condition))                                                                                          \
    {                                                                                                          \
      std::cerr << "Assertion failed: (" << #condition << ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
      exit(1);                                                                                                 \
    }                                                                                                          \
  } while (0)

#define ASSERT_EQ(val1, val2)                                                                                                \
  do                                                                                                                         \
  {                                                                                                                          \
    if ((val1) != (val2))                                                                                                    \
    {                                                                                                                        \
      std::cerr << "Assertion failed: " << (val1) << " != " << (val2) << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
      exit(1);                                                                                                               \
    }                                                                                                                        \
  } while (0)

// Standard Mock transport
#include "../include/lucp/transport.hpp"
#include <vector>

struct SentPacket
{
  std::vector<uint8_t> data;
  uint32_t ip;
  uint16_t port;
};

class MockTransport : public lucp::ITransport
{
public:
  std::vector<SentPacket> sent_packets;
  uint32_t m_time = 0;
  int send_result = 0;

  int lucp_error_count = 0;
  int last_lucp_error = lucp::OK;

  int send(const uint8_t *buf, uint16_t len, uint32_t ip, uint16_t port) override
  {
    if (send_result < 0)
      return send_result;
    sent_packets.push_back({std::vector<uint8_t>(buf, buf + len), ip, port});
    return len;
  }

  struct IncomingPacket
  {
    std::vector<uint8_t> data;
    uint32_t ip;
    uint16_t port;
  };
  std::vector<IncomingPacket> incoming_packets;

  void queue_packet(const uint8_t *buf, uint16_t len, uint32_t ip, uint16_t port)
  {
    incoming_packets.push_back({std::vector<uint8_t>(buf, buf + len), ip, port});
  }

  int receive(uint8_t *buf, uint16_t max_len, uint32_t &src_ip, uint16_t &src_port) override
  {
    if (incoming_packets.empty())
      return 0;
    auto packet = incoming_packets.front();
    incoming_packets.erase(incoming_packets.begin());

    // Fail loudly if a queued test packet exceeds the node's buffer.
    // This catches misconfigured tests rather than silently truncating data.
    if (packet.data.size() > max_len)
    {
      std::cerr << "MockTransport::receive: packet size " << packet.data.size()
                << " exceeds buffer size " << max_len << "\n";
      return -1;
    }

    std::memcpy(buf, packet.data.data(), packet.data.size());
    src_ip = packet.ip;
    src_port = packet.port;
    return static_cast<int>(packet.data.size());
  }

  uint32_t now_ms() override
  {
    return m_time;
  }

  void log_error(int error_code, uint32_t src_ip, uint16_t src_port) override
  {
    (void)src_ip;
    (void)src_port;
    lucp_error_count++;
    last_lucp_error = error_code;
  }
};
