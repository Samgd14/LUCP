#include "../include/lucp/node.hpp"
#include "test_utils.hpp"
#include <iostream>

using namespace lucp;

class SimpleMessage : public IMessage {
public:
    uint8_t id() const override { return 10; }
    uint16_t size() const override { return 4; }
    bool ack_required() const override { return false; }
    int handled_count = 0;

    int handle(const uint8_t* payload, uint16_t size) override {
        handled_count++;
        return OK;
    }
};

class LargeMessage : public IMessage {
public:
    uint8_t id() const override { return 20; }
    uint16_t size() const override { return 1024; } // Beyond MaxPayloadSize in default node
    bool ack_required() const override { return false; }
};

class ZeroSizedMessage : public IMessage {
public:
    uint8_t id() const override { return 30; }
    uint16_t size() const override { return 0; } // Invalid!
    bool ack_required() const override { return false; }
};

void test_registration() {
    MockTransport trans;
    Node<256, 16, 4, 128> node(trans); // MaxPayloadSize = 128

    SimpleMessage sm10;
    ASSERT_EQ(node.register_message(&sm10), OK);

    // Redundant ID / zero sized / out-of-range should fail
    LargeMessage lm;
    ASSERT_EQ(node.register_message(&lm), ERR_BAD_ARG); 

    ZeroSizedMessage zm;
    ASSERT_EQ(node.register_message(&zm), ERR_BAD_ARG);

    SimpleMessage sm_id0;
    // mock an ID of 0
    class BadMSG : public SimpleMessage { uint8_t id() const override { return 0; } } badmsg;
    ASSERT_EQ(node.register_message(&badmsg), ERR_INVALID_ID);
}

void test_validation() {
    MockTransport trans;
    Node<256, 16, 4, 128> node(trans);
    SimpleMessage sm10;
    node.register_message(&sm10);

    // Invalid Magic
    uint8_t bad_magic[] = {0x00, 0x51, 10, 0, 0, 0, 0, 0};
    node.process_packet(bad_magic, 8, 0, 0);
    ASSERT_EQ(sm10.handled_count, 0);

    // Correct magic, Correct length
    uint8_t good[] = {MAGIC_0, MAGIC_1, 10, 0, 0, 0, 0, 0};
    node.process_packet(good, 8, 0, 0);
    ASSERT_EQ(sm10.handled_count, 1);

    // Unregistered ID
    uint8_t unknown[] = {MAGIC_0, MAGIC_1, 99, 0, 0, 0, 0, 0};
    node.process_packet(unknown, 8, 0, 0);
    ASSERT_EQ(trans.unknown_count, 1);

    // Wrong Payload Size
    uint8_t bad_size[] = {MAGIC_0, MAGIC_1, 10, 0, 0, 0, 0}; // 3 byte payload
    node.process_packet(bad_size, 7, 0, 0);
    ASSERT_EQ(trans.rejected_count, 1);
    ASSERT_EQ(sm10.handled_count, 1); // Still 1
}

int main() {
    test_registration();
    test_validation();
    std::cout << "test_core PASSED\n";
    return 0;
}
