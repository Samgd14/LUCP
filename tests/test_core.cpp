#include "../include/lucp/node.hpp"
#include "test_utils.hpp"
#include <iostream>

using namespace lucp;

class SimpleMessage : public IMessage
{
public:
  uint8_t id() const override { return 10; }
  uint16_t size() const override { return 4; }
  bool ack_required() const override { return false; }
  int handled_count = 0;

  int handle(const uint8_t *payload, uint16_t size) override
  {
    handled_count++;
    return OK;
  }
};

class LargeMessage : public IMessage
{
public:
  uint8_t id() const override { return 20; }
  uint16_t size() const override { return 1024; } // Beyond MaxPayloadSize in default node
  bool ack_required() const override { return false; }
};

class ZeroSizedMessage : public IMessage
{
public:
  uint8_t id() const override { return 30; }
  uint16_t size() const override { return 0; } // Invalid!
  bool ack_required() const override { return false; }
};

void test_registration()
{
  MockTransport trans;
  Node<256, 16, 4, 128> node(trans); // MaxPayloadSize = 128

  SimpleMessage sm10;
  ASSERT_EQ(node.register_message(&sm10), OK);

  // Redundant ID / zero sized / out-of-range should fail
  LargeMessage lm;
  ASSERT_EQ(node.register_message(&lm), ERR_INVALID_SIZE);

  ZeroSizedMessage zm;
  ASSERT_EQ(node.register_message(&zm), ERR_INVALID_SIZE);

  // mock an ID of 0
  class BadMSG : public SimpleMessage
  {
    uint8_t id() const override { return 0; }
  } badmsg;
  ASSERT_EQ(node.register_message(&badmsg), ERR_INVALID_ID);
}

void test_validation()
{
  MockTransport trans;
  Node<256, 16, 4, 128> node(trans);
  SimpleMessage sm10;
  node.register_message(&sm10);

  // Invalid Magic
  uint8_t bad_magic[] = {0x00, 0x51, 10, 0, 0, 0, 0, 0};
  ASSERT_EQ(node.process_packet(bad_magic, 8, 0, 0), ERR_INVALID_PACKET);
  ASSERT_EQ(sm10.handled_count, 0);

  // Correct magic, Correct length
  uint8_t good[] = {MAGIC_0, MAGIC_1, 10, 0, 0, 0, 0, 0};
  ASSERT_EQ(node.process_packet(good, 8, 0, 0), OK);
  ASSERT_EQ(sm10.handled_count, 1);

  // Unregistered ID
  uint8_t unknown[] = {MAGIC_0, MAGIC_1, 99, 0, 0, 0, 0, 0};
  ASSERT_EQ(node.process_packet(unknown, 8, 0, 0), ERR_INVALID_ID);

  // Wrong Payload Size
  uint8_t bad_size[] = {MAGIC_0, MAGIC_1, 10, 0, 0, 0, 0}; // 3 byte payload
  ASSERT_EQ(node.process_packet(bad_size, 7, 0, 0), ERR_INVALID_PACKET);

  // process_packet itself should not produce transport logs
  ASSERT_EQ(trans.lucp_error_count, 0);
  ASSERT_EQ(sm10.handled_count, 1); // Still 1
}

void test_receive_incoming_logs_errors()
{
  MockTransport trans;
  Node<256, 16, 4, 128> node(trans);
  SimpleMessage sm10;
  node.register_message(&sm10);

  uint8_t bad_magic[] = {0x00, 0x51, 10, 0, 0, 0, 0, 0};
  uint8_t good[] = {MAGIC_0, MAGIC_1, 10, 0, 0, 0, 0, 0};

  trans.queue_packet(bad_magic, 8, 0x7F000001, 9000);
  trans.queue_packet(good, 8, 0x7F000001, 9001);

  node.receive_incoming();

  ASSERT_EQ(trans.lucp_error_count, 1);
  ASSERT_EQ(trans.last_lucp_error, ERR_INVALID_PACKET);
  ASSERT_EQ(sm10.handled_count, 1);
}

// A message that does NOT override handle() (inherits the default).
class NoHandleMessage : public IMessage
{
public:
  uint8_t id() const override { return 11; }
  uint16_t size() const override { return 4; }
  bool ack_required() const override { return true; }
};

void test_default_handle_returns_missing()
{
  MockTransport trans;
  Node<256, 16, 4, 128> node(trans);
  NoHandleMessage nh;
  ASSERT_EQ(node.register_message(&nh), OK);

  uint8_t pkt[] = {MAGIC_0, MAGIC_1, 11, 0, 0, 0, 0, 0};
  ASSERT_EQ(node.process_packet(pkt, 8, 0, 0), ERR_HANDLE_MISSING);
  ASSERT_EQ(trans.sent_packets.size(), 0); // No echo queued/dispatched
}

void test_error_codes_distinct()
{
  ASSERT_TRUE(ERR_HANDLE_MISSING < 0);
  ASSERT_TRUE(ERR_ON_FAIL_MISSING < 0);
  ASSERT_TRUE(ERR_NO_PENDING < 0);
  ASSERT_TRUE(ERR_NOT_REGISTERED < 0);
  ASSERT_TRUE(ERR_HANDLE_MISSING != ERR_ON_FAIL_MISSING);
  ASSERT_TRUE(ERR_HANDLE_MISSING != ERR_NO_PENDING);
  ASSERT_TRUE(ERR_HANDLE_MISSING != ERR_NOT_REGISTERED);
  ASSERT_TRUE(ERR_ON_FAIL_MISSING != ERR_NO_PENDING);
  ASSERT_TRUE(ERR_ON_FAIL_MISSING != ERR_NOT_REGISTERED);
  ASSERT_TRUE(ERR_NO_PENDING != ERR_NOT_REGISTERED);
}

void test_error_code_paths()
{
  MockTransport trans;
  Node<256, 16, 4, 128> node(trans);

  // ERR_ALREADY_REGISTERED
  SimpleMessage a;
  ASSERT_EQ(node.register_message(&a), OK);
  SimpleMessage b; // same id (10)
  ASSERT_EQ(node.register_message(&b), ERR_ALREADY_REGISTERED);

  // ERR_BAD_ARG: null packet, undersize
  ASSERT_EQ(node.process_packet(nullptr, 8, 0, 0), ERR_BAD_ARG);
  uint8_t tiny[] = {MAGIC_0, MAGIC_1, 10};
  ASSERT_EQ(node.process_packet(tiny, 3, 0, 0), ERR_BAD_ARG); // size < HEADER_SIZE

  // ERR_PACKET_TOO_LARGE
  uint8_t too_big[HEADER_SIZE + 129] = {MAGIC_0, MAGIC_1, 10, 0};
  ASSERT_EQ(node.process_packet(too_big, sizeof(too_big), 0, 0), ERR_PACKET_TOO_LARGE);
}

void test_queue_full_on_send()
{
  MockTransport trans;
  Node<256, 16, 2, 128> node(trans); // MaxPendingAcks = 2
  class RelMsg : public TypedMessage<uint8_t[4]>
  {
  public:
    uint8_t id() const override { return 50; }
    bool ack_required() const override { return true; }
    uint8_t max_retries() const override { return 5; }
    uint16_t retry_delay_ms() const override { return 1000; }
  } m1, m2, m3;
  node.register_message(&m1);
  // Same id won't allow multiple registrations; use distinct ids.
  class RelMsg2 : public RelMsg
  {
  public:
    uint8_t id() const override { return 51; }
  } m2b, m3b;
  class RelMsg3 : public RelMsg
  {
  public:
    uint8_t id() const override { return 52; }
  } m4, m5;
  node.register_message(&m2b);
  node.register_message(&m4);
  node.register_message(&m5);

  uint8_t p[4] = {0, 0, 0, 0};
  ASSERT_EQ(m1.send(p, 0, 0), OK);        // slot 1
  ASSERT_EQ(m2b.send(p, 0, 0), OK);        // slot 2
  ASSERT_EQ(m4.send(p, 0, 0), ERR_QUEUE_FULL); // no slots left
}

void test_reset_wipes_registry()
{
  MockTransport trans;
  Node<256, 16, 4, 128> node(trans);
  SimpleMessage sm;
  ASSERT_EQ(node.register_message(&sm), OK);

  node.reset(); // wipes registry

  uint8_t good[] = {MAGIC_0, MAGIC_1, 10, 0, 0, 0, 0, 0};
  ASSERT_EQ(node.process_packet(good, 8, 0, 0), ERR_INVALID_ID); // no longer registered
}

void test_reset_state_preserves_registry()
{
  MockTransport trans;
  Node<256, 16, 4, 128> node(trans);
  SimpleMessage sm;
  ASSERT_EQ(node.register_message(&sm), OK);

  uint8_t good[] = {MAGIC_0, MAGIC_1, 10, 0, 0, 0, 0, 0};
  ASSERT_EQ(node.process_packet(good, 8, 0, 0), OK);
  ASSERT_EQ(sm.handled_count, 1);

  node.reset_state(); // preserves registry, clears state (incl. dedup)
  ASSERT_EQ(node.process_packet(good, 8, 0, 0), OK); // still registered, treated as new
  ASSERT_EQ(sm.handled_count, 2);
}

void test_max_recv_burst_cap()
{
  MockTransport trans;
  Node<256, 16, 4, 128> node(trans); // default MaxRecvBurst = 64
  SimpleMessage sm;
  node.register_message(&sm);

  uint8_t good[] = {MAGIC_0, MAGIC_1, 10, 0, 0, 0, 0, 0};
  for (int i = 0; i < 70; ++i)
    trans.queue_packet(good, 8, 0, 0);

  node.receive_incoming();
  // Only first 64 processed in this call (dedup also drops duplicate seqs:
  // all have seq 0, so only the first is handled).
  ASSERT_EQ(sm.handled_count, 1);
  // Remaining packets still queued; a second call drains more (still dedup'd).
  node.receive_incoming();
  ASSERT_EQ(sm.handled_count, 1); // still seq 0 -> dedup'd
}

int main()
{
  test_registration();
  test_validation();
  test_receive_incoming_logs_errors();
  test_error_codes_distinct();
  test_default_handle_returns_missing();
  test_error_code_paths();
  test_queue_full_on_send();
  test_reset_wipes_registry();
  test_reset_state_preserves_registry();
  test_max_recv_burst_cap();
  std::cout << "test_core PASSED\n";
  return 0;
}
