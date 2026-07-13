#include "../include/lucp/message.hpp"
#include "../include/lucp/node.hpp"
#include "test_utils.hpp"
#include <cstring>
#include <iostream>

using namespace lucp;

struct TestPayload {
    uint32_t a;
    float b;
};

class IntMessage : public TypedMessage<TestPayload> {
public:
    uint8_t id() const override { return 5; }
    bool ack_required() const override { return false; }

    TestPayload last_payload = {0, 0.0f};
    int ok_counts = 0;
    int err_counts = 0;

    int handle(const uint8_t* payload, uint16_t size) override {
        if (size != sizeof(TestPayload)) {
            err_counts++;
            return ERR_BAD_ARG;
        }
        std::memcpy(&last_payload, payload, size);
        ok_counts++;
        return OK;
    }
};

void test_typed_send() {
    MockTransport trans;
    Node<> node(trans);
    IntMessage imsg;
    ASSERT_EQ(node.register_message(&imsg), OK);

    TestPayload tx = {42, 3.14f};
    ASSERT_EQ(imsg.send(tx, 1234, 8080), OK);

    ASSERT_EQ(trans.sent_packets.size(), 1);
    
    // Validate payload layout inside sent packet
    ASSERT_EQ(trans.sent_packets[0].data.size(), HEADER_SIZE + sizeof(TestPayload));
    
    TestPayload rx;
    std::memcpy(&rx, trans.sent_packets[0].data.data() + HEADER_SIZE, sizeof(TestPayload));
    ASSERT_EQ(rx.a, 42);
    // Use an epsilon or just strict equality for float assignments without math ops
    ASSERT_EQ(rx.b, 3.14f);
}

void test_handle_called_with_oop_instance() {
    MockTransport trans;
    Node<> node(trans);
    IntMessage imsg;
    node.register_message(&imsg);

    uint8_t packet[HEADER_SIZE + sizeof(TestPayload)];
    packet[0] = MAGIC_0;
    packet[1] = MAGIC_1;
    packet[2] = 5;
    packet[3] = 0;
    
    TestPayload px = {100, -5.0f};
    std::memcpy(&packet[HEADER_SIZE], &px, sizeof(TestPayload));

    node.process_packet(packet, sizeof(packet), 0, 0);
    ASSERT_EQ(imsg.ok_counts, 1);
    ASSERT_EQ(imsg.err_counts, 0);
    ASSERT_EQ(imsg.last_payload.a, 100);
    ASSERT_EQ(imsg.last_payload.b, -5.0f);
}

class UnregisteredTyped : public TypedMessage<uint32_t>
{
public:
  uint8_t id() const override { return 7; }
  bool ack_required() const override { return false; }
};

void test_typed_send_not_registered() {
    MockTransport trans;
    Node<> node(trans);
    UnregisteredTyped msg; // NOT registered
    uint32_t v = 1;
    ASSERT_EQ(msg.send(v, 0, 0), ERR_NOT_REGISTERED);
    ASSERT_EQ(trans.sent_packets.size(), 0);
}

void test_wire_format_byte_offsets()
{
  MockTransport trans;
  Node<> node(trans);
  IntMessage imsg;
  node.register_message(&imsg);

  TestPayload tx = {7, 1.5f};
  ASSERT_EQ(imsg.send(tx, 0x01020304, 5678), OK);
  ASSERT_EQ(trans.sent_packets.size(), 1);

  const auto &d = trans.sent_packets[0].data;
  ASSERT_EQ(d.size(), HEADER_SIZE + sizeof(TestPayload));
  ASSERT_EQ(d[0], MAGIC_0);
  ASSERT_EQ(d[1], MAGIC_1);
  ASSERT_EQ(d[2], 5);     // msg_id at byte 2
  ASSERT_EQ(d[3], 0);     // seq_id at byte 3 (first send)
  ASSERT_EQ(trans.sent_packets[0].ip, 0x01020304);
  ASSERT_EQ(trans.sent_packets[0].port, 5678);

  TestPayload rx;
  std::memcpy(&rx, d.data() + HEADER_SIZE, sizeof(TestPayload));
  ASSERT_EQ(rx.a, 7);
  ASSERT_EQ(rx.b, 1.5f);
}

int main() {
    test_typed_send();
    test_handle_called_with_oop_instance();
    test_typed_send_not_registered();
    test_wire_format_byte_offsets();
    std::cout << "test_message PASSED\n";
    return 0;
}
