#include "../include/lucp/node.hpp"
#include "test_utils.hpp"
#include <iostream>

using namespace lucp;

class ReliableMessage : public TypedMessage<uint8_t[4]>
{
public:
  uint8_t id() const override { return 100; }
  bool ack_required() const override { return true; }
  uint8_t max_retries() const override { return 2; }
  uint16_t retry_delay_ms() const override { return 100; }

  int fail_count = 0;
  int handled_count = 0;

  int handle(const uint8_t *payload, uint16_t size) override
  {
    handled_count++;
    return OK;
  }

  int on_fail() override
  {
    fail_count++;
    return OK;
  }
};

void test_ack_workflow()
{
  MockTransport trans;
  Node<> node(trans);
  ReliableMessage msg;
  node.register_message(&msg);

  uint8_t payload[4] = {1, 2, 3, 4};
  ASSERT_EQ(msg.send(payload, 0xC0A80001, 9000), OK);

  ASSERT_EQ(trans.sent_packets.size(), 1);

  // time passes, retry fires
  trans.m_time += 150;
  node.ack_tick();

  ASSERT_EQ(trans.sent_packets.size(), 2); // Retried once!

  // simulate ACK arriving back
  uint8_t ack_packet[HEADER_SIZE];
  ack_packet[0] = MAGIC_0;
  ack_packet[1] = MAGIC_1;
  ack_packet[2] = msg.id();
  ack_packet[3] = trans.sent_packets[0].data[3]; // seq_id

  // Process ACK!
  node.process_packet(ack_packet, HEADER_SIZE, 0xC0A80001, 9000);

  // tick again -> no more retries
  trans.m_time += 150;
  trans.m_time += 150;
  node.ack_tick();

  ASSERT_EQ(msg.fail_count, 0);            // Didn't fail!
  ASSERT_EQ(trans.sent_packets.size(), 2); // No more retries sent!
}

void test_ack_exhaustion()
{
  MockTransport trans;
  Node<> node(trans);
  ReliableMessage msg;
  node.register_message(&msg);

  uint8_t payload[4] = {1, 2, 3, 4};
  msg.send(payload, 0, 0);

  // Initial transmission
  ASSERT_EQ(trans.sent_packets.size(), 1);

  // T1: 1st Retry
  trans.m_time += 150;
  node.ack_tick();
  ASSERT_EQ(trans.sent_packets.size(), 2);
  ASSERT_EQ(msg.fail_count, 0);

  // T2: 2nd Retry
  trans.m_time += 150;
  node.ack_tick();
  ASSERT_EQ(trans.sent_packets.size(), 3);
  ASSERT_EQ(msg.fail_count, 0);

  // T3: Exhausted
  trans.m_time += 150;
  node.ack_tick();
  ASSERT_EQ(msg.fail_count, 1);
  ASSERT_EQ(trans.sent_packets.size(), 3); // No more TX
}

void test_echo_response()
{
  MockTransport trans;
  Node<> node(trans);
  ReliableMessage msg;
  node.register_message(&msg);

  // Incoming payload packet demanding an ACK echo
  uint8_t rx[HEADER_SIZE + 4];
  rx[0] = MAGIC_0;
  rx[1] = MAGIC_1;
  rx[2] = 100;
  rx[3] = 42; // seq_id

  // Dispatch
  node.process_packet(rx, sizeof(rx), 0xC0A80201, 1000);

  ASSERT_EQ(msg.handled_count, 1);
  ASSERT_EQ(trans.sent_packets.size(), 0); // Before flush, no echo sent!

  // flush_echo_queue dispatches the queued echo ACK
  node.flush_echo_queue();
  ASSERT_EQ(trans.sent_packets.size(), 1);
  ASSERT_EQ(trans.sent_packets[0].data.size(), HEADER_SIZE); // Echo is header-only!
  ASSERT_EQ(trans.sent_packets[0].data[3], 42);              // Sequence mirrored
}

// A reliable message that does NOT override on_fail() (inherits the default).
class NoOnFailMessage : public TypedMessage<uint8_t[4]>
{
public:
  uint8_t id() const override { return 101; }
  bool ack_required() const override { return true; }
  uint8_t max_retries() const override { return 1; }
  uint16_t retry_delay_ms() const override { return 100; }
  int handle(const uint8_t *, uint16_t) override { return OK; }
};

void test_default_on_fail_returns_missing()
{
  MockTransport trans;
  Node<> node(trans);
  NoOnFailMessage msg;
  node.register_message(&msg);

  uint8_t payload[4] = {1, 2, 3, 4};
  ASSERT_EQ(msg.send(payload, 0, 0), OK);
  ASSERT_EQ(trans.sent_packets.size(), 1);

  // T1: 1st retry
  trans.m_time += 150;
  node.ack_tick();
  ASSERT_EQ(trans.sent_packets.size(), 2);

  // T2: exhausted -> on_fail() default returns ERR_ON_FAIL_MISSING, logged
  trans.m_time += 150;
  node.ack_tick();
  ASSERT_EQ(trans.sent_packets.size(), 2);
  ASSERT_TRUE(trans.lucp_error_count > 0);
  ASSERT_EQ(trans.last_lucp_error, ERR_ON_FAIL_MISSING);
}

// handle() returns a POSITIVE value; ACK must still be echoed (B8).
class PositiveHandleMessage : public TypedMessage<uint8_t[4]>
{
public:
  uint8_t id() const override { return 102; }
  bool ack_required() const override { return true; }
  int handle(const uint8_t *, uint16_t) override { return 7; } // positive, non-OK
};

void test_positive_handle_still_acks()
{
  MockTransport trans;
  Node<> node(trans);
  PositiveHandleMessage msg;
  node.register_message(&msg);

  uint8_t rx[HEADER_SIZE + 4] = {MAGIC_0, MAGIC_1, 102, 9, 0, 0, 0, 0};
  ASSERT_EQ(node.process_packet(rx, sizeof(rx), 0xC0A80301, 1000), OK);

  // Echo must have been queued and is dispatched on flush.
  node.flush_echo_queue();
  ASSERT_EQ(trans.sent_packets.size(), 1);
  ASSERT_EQ(trans.sent_packets[0].data.size(), HEADER_SIZE);
  ASSERT_EQ(trans.sent_packets[0].data[3], 9);
}

void test_spurious_ack_returns_no_pending()
{
  MockTransport trans;
  Node<> node(trans);
  ReliableMessage msg; // ack_required, id 100
  node.register_message(&msg);

  // ACK for a seq/peer with NO pending record.
  uint8_t ack[HEADER_SIZE] = {MAGIC_0, MAGIC_1, 100, 77};
  ASSERT_EQ(node.process_packet(ack, HEADER_SIZE, 0xC0A80401, 9000), ERR_NO_PENDING);

  // Now a real send + matching ACK returns OK.
  uint8_t payload[4] = {1, 2, 3, 4};
  ASSERT_EQ(msg.send(payload, 0xC0A80401, 9000), OK);
  uint8_t realAck[HEADER_SIZE] = {MAGIC_0, MAGIC_1, 100, trans.sent_packets[0].data[3]};
  ASSERT_EQ(node.process_packet(realAck, HEADER_SIZE, 0xC0A80401, 9000), OK);
}

int main()
{
  test_ack_workflow();
  test_ack_exhaustion();
  test_echo_response();
  test_default_on_fail_returns_missing();
  test_positive_handle_still_acks();
  test_spurious_ack_returns_no_pending();
  std::cout << "test_ack_echo PASSED\n";
  return 0;
}
