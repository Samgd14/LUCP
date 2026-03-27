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

int main() {
    test_typed_send();
    test_handle_called_with_oop_instance();
    std::cout << "test_message PASSED\n";
    return 0;
}
