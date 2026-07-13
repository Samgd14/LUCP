// test_node.cpp
// Integration test of the full Node send/receive/retry/echo flow, with assertions.

#include "../include/lucp/node.hpp"
#include "test_utils.hpp"
#include <cstring>
#include <iostream>

struct MotorPayload
{
  float torque;
  float position;
};

class MotorCmdMessage : public lucp::TypedMessage<MotorPayload>
{
public:
  uint8_t id() const override { return 1; }
  bool ack_required() const override { return true; }
  uint8_t max_retries() const override { return 3; }
  uint16_t retry_delay_ms() const override { return 100; }
  int handle(const uint8_t *payload, uint16_t size) override
  {
    std::memcpy(&m_last, payload, size);
    m_handled++;
    return lucp::OK;
  }
  int on_fail() override
  {
    m_failed++;
    return lucp::OK;
  }
  MotorPayload m_last = {0.0f, 0.0f};
  int m_handled = 0;
  int m_failed = 0;
};

int main()
{
  MockTransport transport;
  lucp::Node<> node(transport);

  MotorCmdMessage motor_cmd;
  ASSERT_EQ(node.register_message(&motor_cmd), lucp::OK);

  // 1. Typed send builds a correctly framed packet.
  MotorPayload tx_cmd = {10.5f, -3.14f};
  ASSERT_EQ(motor_cmd.send(tx_cmd, 0xC0A8010A, 9000), lucp::OK);
  ASSERT_EQ(transport.sent_packets.size(), 1);
  ASSERT_EQ(transport.sent_packets[0].data.size(), lucp::HEADER_SIZE + sizeof(MotorPayload));
  ASSERT_EQ(transport.sent_packets[0].data[0], lucp::MAGIC_0);
  ASSERT_EQ(transport.sent_packets[0].data[1], lucp::MAGIC_1);
  ASSERT_EQ(transport.sent_packets[0].data[2], 1); // msg_id
  ASSERT_EQ(transport.sent_packets[0].data[3], 0); // first seq
  ASSERT_EQ(transport.sent_packets[0].ip, 0xC0A8010A);
  ASSERT_EQ(transport.sent_packets[0].port, 9000);

  // 2. Advance time -> retries fire (max_retries 3 -> 3 retries, no failure yet).
  for (int i = 0; i < 3; ++i)
  {
    transport.m_time += 150;
    node.ack_tick();
  }
  ASSERT_EQ(transport.sent_packets.size(), 4); // initial + 3 retries
  ASSERT_EQ(motor_cmd.m_failed, 0);

  // 3. Receive a data packet back (echo queued, not yet dispatched).
  uint8_t rx_packet[lucp::HEADER_SIZE + sizeof(MotorPayload)];
  rx_packet[0] = lucp::MAGIC_0;
  rx_packet[1] = lucp::MAGIC_1;
  rx_packet[2] = 1;  // msg_id
  rx_packet[3] = 42; // seq_id
  std::memcpy(&rx_packet[lucp::HEADER_SIZE], &tx_cmd, sizeof(MotorPayload));
  ASSERT_EQ(node.process_packet(rx_packet, sizeof(rx_packet), 0xC0A8010A, 9000), lucp::OK);
  ASSERT_EQ(motor_cmd.m_handled, 1);
  ASSERT_EQ(motor_cmd.m_last.torque, 10.5f);
  ASSERT_EQ(motor_cmd.m_last.position, -3.14f);
  ASSERT_EQ(transport.sent_packets.size(), 4); // echo not flushed yet

  // 4. receive_incoming() with two packets using DISTINCT seqs (dedup is per msg_id).
  uint8_t rx_b[lucp::HEADER_SIZE + sizeof(MotorPayload)];
  std::memcpy(rx_b, rx_packet, sizeof(rx_packet));
  rx_b[3] = 43; // distinct seq
  uint8_t rx_c[lucp::HEADER_SIZE + sizeof(MotorPayload)];
  std::memcpy(rx_c, rx_packet, sizeof(rx_packet));
  rx_c[3] = 44; // distinct seq
  transport.queue_packet(rx_b, sizeof(rx_b), 0xC0A8010A, 9001);
  transport.queue_packet(rx_c, sizeof(rx_c), 0xC0A8010B, 9002);
  node.receive_incoming();
  ASSERT_EQ(motor_cmd.m_handled, 3); // 1 from step 3 + 2 here

  // 5. Flush echoes: 3 echoes queued (steps 3 + 4).
  node.flush_echo_queue();
  ASSERT_EQ(transport.sent_packets.size(), 7); // 4 sends + 3 echoes

  std::cout << "test_node PASSED\n";
  return 0;
}