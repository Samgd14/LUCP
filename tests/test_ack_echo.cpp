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

void test_retry_send_failure_not_consumed()
{
  MockTransport trans;
  Node<> node(trans);
  ReliableMessage msg; // max_retries 2, retry_delay 100
  node.register_message(&msg);

  uint8_t payload[4] = {1, 2, 3, 4};
  ASSERT_EQ(msg.send(payload, 0, 0), OK);
  ASSERT_EQ(trans.sent_packets.size(), 1); // initial send succeeded

  // Make the transport fail for the upcoming retry.
  trans.send_result = -3; // ERR_PAL_SEND
  trans.m_time += 150;
  node.ack_tick();

  // Retry send failed: NOT added to sent_packets, and logged.
  ASSERT_EQ(trans.sent_packets.size(), 1);
  ASSERT_TRUE(trans.lucp_error_count > 0);

  // Retry was NOT consumed: restore transport and tick again -> retry now fires.
  trans.send_result = 0;
  trans.m_time += 150;
  node.ack_tick();
  ASSERT_EQ(trans.sent_packets.size(), 2);
}

// A reliable message that counts handle() calls.
class CountingReliable : public TypedMessage<uint8_t[4]>
{
public:
  uint8_t id() const override { return 103; }
  bool ack_required() const override { return true; }
  int handle(const uint8_t *, uint16_t) override { handled_count++; return OK; }
  int handled_count = 0;
};

void test_duplicate_retransmit_not_rehandled_but_acked()
{
  MockTransport trans;
  Node<> node(trans);
  CountingReliable msg;
  node.register_message(&msg);

  uint8_t rx[HEADER_SIZE + 4] = {MAGIC_0, MAGIC_1, 103, 5, 1, 2, 3, 4};
  ASSERT_EQ(node.process_packet(rx, sizeof(rx), 0xC0A80501, 1000), OK);
  ASSERT_EQ(msg.handled_count, 1);

  // Same seq again (a retransmit): NOT re-handled, but ACK still echoed.
  ASSERT_EQ(node.process_packet(rx, sizeof(rx), 0xC0A80501, 1000), OK);
  ASSERT_EQ(msg.handled_count, 1);

  node.flush_echo_queue();
  // Two echoes queued (one per received packet, including the duplicate).
  ASSERT_EQ(trans.sent_packets.size(), 2);
  ASSERT_EQ(trans.sent_packets[0].data[3], 5);
  ASSERT_EQ(trans.sent_packets[1].data[3], 5);
}

void test_default_rejects_out_of_order_older()
{
  MockTransport trans;
  Node<> node(trans);
  CountingReliable msg;
  node.register_message(&msg);

  // Receive seq 10 first (newest so far).
  uint8_t r10[HEADER_SIZE + 4] = {MAGIC_0, MAGIC_1, 103, 10, 1, 2, 3, 4};
  ASSERT_EQ(node.process_packet(r10, sizeof(r10), 0, 0), OK);
  ASSERT_EQ(msg.handled_count, 1);

  // Older seq 9 (out-of-order, behind 10): dropped, not handled.
  uint8_t r9[HEADER_SIZE + 4] = {MAGIC_0, MAGIC_1, 103, 9, 1, 2, 3, 4};
  ASSERT_EQ(node.process_packet(r9, sizeof(r9), 0, 0), OK);
  ASSERT_EQ(msg.handled_count, 1);

  // Newer seq 11: handled.
  uint8_t r11[HEADER_SIZE + 4] = {MAGIC_0, MAGIC_1, 103, 11, 1, 2, 3, 4};
  ASSERT_EQ(node.process_packet(r11, sizeof(r11), 0, 0), OK);
  ASSERT_EQ(msg.handled_count, 2);
}

void test_wrap_around_new_packet_handled()
{
  MockTransport trans;
  Node<> node(trans);
  CountingReliable msg;
  node.register_message(&msg);

  // Last seen = 250.
  uint8_t r250[HEADER_SIZE + 4] = {MAGIC_0, MAGIC_1, 103, 250, 1, 2, 3, 4};
  node.process_packet(r250, sizeof(r250), 0, 0);
  ASSERT_EQ(msg.handled_count, 1);

  // seq 5 after 250: diff (250-5)&0xFF = 245 > 127 -> treated as newer (wrapped). Handled.
  uint8_t r5[HEADER_SIZE + 4] = {MAGIC_0, MAGIC_1, 103, 5, 1, 2, 3, 4};
  ASSERT_EQ(node.process_packet(r5, sizeof(r5), 0, 0), OK);
  ASSERT_EQ(msg.handled_count, 2);
}

void test_reset_state_clears_dedup()
{
  MockTransport trans;
  Node<> node(trans);
  CountingReliable msg;
  node.register_message(&msg);

  uint8_t rx[HEADER_SIZE + 4] = {MAGIC_0, MAGIC_1, 103, 5, 1, 2, 3, 4};
  node.process_packet(rx, sizeof(rx), 0, 0);
  ASSERT_EQ(msg.handled_count, 1);

  node.reset_state(); // preserves registry, clears dedup + seq + ack state

  // Same seq 5 again after reset -> treated as new, handled.
  ASSERT_EQ(node.process_packet(rx, sizeof(rx), 0, 0), OK);
  ASSERT_EQ(msg.handled_count, 2);
}

class EchoTargetMessage : public TypedMessage<uint8_t[4]>
{
public:
  uint8_t id() const override { return 104; }
  bool ack_required() const override { return true; }
  int handle(const uint8_t *, uint16_t) override { handled_count++; return OK; }
  int handled_count = 0;
};

// A reliable message that accepts out-of-order (older) packets.
class AcceptingReliable : public TypedMessage<uint8_t[4]>
{
public:
  uint8_t id() const override { return 105; }
  bool ack_required() const override { return true; }
  bool reject_out_of_order() const override { return false; }
  int handle(const uint8_t *, uint16_t) override { handled_count++; return OK; }
  int handled_count = 0;
};

void test_accept_out_of_order_handles_older()
{
  MockTransport trans;
  Node<> node(trans);
  AcceptingReliable msg;
  node.register_message(&msg);

  // seq 50 first (newest so far) -> handled.
  uint8_t r50[HEADER_SIZE + 4] = {MAGIC_0, MAGIC_1, 105, 50, 1, 2, 3, 4};
  ASSERT_EQ(node.process_packet(r50, sizeof(r50), 0, 0), OK);
  ASSERT_EQ(msg.handled_count, 1);

  // Older seq 40 (out-of-order, behind 50): accepted (not dropped) -> handled, ACKed.
  uint8_t r40[HEADER_SIZE + 4] = {MAGIC_0, MAGIC_1, 105, 40, 1, 2, 3, 4};
  ASSERT_EQ(node.process_packet(r40, sizeof(r40), 0, 0), OK);
  ASSERT_EQ(msg.handled_count, 2);

  // Newer seq 51 still handled (last_seq did not regress to 40).
  uint8_t r51[HEADER_SIZE + 4] = {MAGIC_0, MAGIC_1, 105, 51, 1, 2, 3, 4};
  ASSERT_EQ(node.process_packet(r51, sizeof(r51), 0, 0), OK);
  ASSERT_EQ(msg.handled_count, 3);

  // Each received packet queued an ACK echo.
  node.flush_echo_queue();
  ASSERT_EQ(trans.sent_packets.size(), 3);
  ASSERT_EQ(trans.sent_packets[0].data[3], 50);
  ASSERT_EQ(trans.sent_packets[1].data[3], 40);
  ASSERT_EQ(trans.sent_packets[2].data[3], 51);
}

void test_echo_queue_full_drops_before_handle()
{
  MockTransport trans;
  Node<256, 2, 4, 256> node(trans); // EchoQueueDepth = 2
  EchoTargetMessage msg;
  node.register_message(&msg);

  uint8_t rx[HEADER_SIZE + 4] = {MAGIC_0, MAGIC_1, 104, 0, 1, 2, 3, 4};
  ASSERT_EQ(node.process_packet(rx, sizeof(rx), 0, 0), OK);        // echo 1
  ASSERT_EQ(node.process_packet(rx, sizeof(rx), 0, 0), OK);        // echo 2 (dup, not re-handled)
  ASSERT_EQ(msg.handled_count, 1);

  // Use a NEW seq so it's not dedup'd, but echo queue is now full (2 entries).
  uint8_t rx2[HEADER_SIZE + 4] = {MAGIC_0, MAGIC_1, 104, 1, 1, 2, 3, 4};
  ASSERT_EQ(node.process_packet(rx2, sizeof(rx2), 0, 0), ERR_QUEUE_FULL);
  ASSERT_EQ(msg.handled_count, 1); // handle() NOT called (dropped before handle)
}

void test_ack_wrong_peer_does_not_clear()
{
  MockTransport trans;
  Node<> node(trans);
  ReliableMessage msg;
  node.register_message(&msg);

  uint8_t payload[4] = {1, 2, 3, 4};
  ASSERT_EQ(msg.send(payload, 0xC0A80601, 9000), OK);
  uint8_t seq = trans.sent_packets[0].data[3];

  // ACK from the WRONG peer -> no match -> ERR_NO_PENDING, pending stays.
  uint8_t ackWrong[HEADER_SIZE] = {MAGIC_0, MAGIC_1, 100, seq};
  ASSERT_EQ(node.process_packet(ackWrong, HEADER_SIZE, 0xC0A80699, 9999), ERR_NO_PENDING);

  // A retry should still fire (pending not cleared).
  trans.m_time += 150;
  node.ack_tick();
  ASSERT_EQ(trans.sent_packets.size(), 2);

  // ACK from the RIGHT peer -> clears -> OK.
  uint8_t ackRight[HEADER_SIZE] = {MAGIC_0, MAGIC_1, 100, seq};
  ASSERT_EQ(node.process_packet(ackRight, HEADER_SIZE, 0xC0A80601, 9000), OK);

  trans.m_time += 150;
  node.ack_tick();
  ASSERT_EQ(trans.sent_packets.size(), 2); // no further retry
}

int main()
{
  test_ack_workflow();
  test_ack_exhaustion();
  test_echo_response();
  test_default_on_fail_returns_missing();
  test_positive_handle_still_acks();
  test_spurious_ack_returns_no_pending();
  test_retry_send_failure_not_consumed();
  test_duplicate_retransmit_not_rehandled_but_acked();
  test_default_rejects_out_of_order_older();
  test_accept_out_of_order_handles_older();
  test_wrap_around_new_packet_handled();
  test_reset_state_clears_dedup();
  test_echo_queue_full_drops_before_handle();
  test_ack_wrong_peer_does_not_clear();
  std::cout << "test_ack_echo PASSED\n";
  return 0;
}
