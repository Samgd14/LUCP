// test_node.cpp
// Integration test / manual verification of the full Node send/receive/retry flow.
// This file uses process_all() / receive_incoming() / ack_tick() (current API).

#include "../include/lucp/node.hpp"
#include "test_utils.hpp"
#include <cstring>
#include <iostream>

// -------------------------------------------------------------
// A Shared Message Definition (could live in shared_messages.hpp)
// -------------------------------------------------------------
struct MotorPayload {
    float torque;
    float position;
};

class MotorCmdMessage : public lucp::IMessage {
public:
    uint8_t  id() const override { return 1; }
    uint16_t size() const override { return sizeof(MotorPayload); }
    bool     ack_required() const override { return true; }
    uint8_t  max_retries() const override { return 3; }
    uint16_t retry_delay_ms() const override { return 100; }

    // Helper syntax for directly sending typed payload
    int send(const MotorPayload& p, uint32_t dest_ip, uint16_t dest_port) {
        return send_raw(reinterpret_cast<const uint8_t*>(&p), dest_ip, dest_port);
    }
};

// -------------------------------------------------------------
// Node-specific Implementation
// -------------------------------------------------------------
class VirtualMotor {
public:
    void set_target(float t, float p) {
        std::cout << "VirtualMotor natively updating: torque=" << t << ", pos=" << p << "\n";
    }
};

class NodeMotorCmd : public MotorCmdMessage {
    VirtualMotor& m_motor;
public:
    NodeMotorCmd(VirtualMotor& motor) : m_motor(motor) {}

    int handle(const uint8_t* payload, uint16_t size) override {
        MotorPayload cmd;
        std::memcpy(&cmd, payload, size);
        m_motor.set_target(cmd.torque, cmd.position);
        return lucp::OK;
    }

    int on_fail() override {
        std::cout << "FAIL CALLBACK: MotorCmd exhausted retries.\n";
        return lucp::OK;
    }
};

// Main verification
int main() {
    std::cout << "--- LUCP C++ OO Node Verification ---\n";

    MockTransport transport;
    lucp::Node<> node(transport);

    // 1. Instantiate systems and messages natively
    VirtualMotor right_leg_motor;
    NodeMotorCmd motor_cmd(right_leg_motor);

    // 2. Register cleanly
    node.register_message(&motor_cmd);

    std::cout << "\n1. Sending a MotorCmd natively via object method\n";
    MotorPayload tx_cmd = { 10.5f, -3.14f };
    // This is the clean, type-safe send!
    motor_cmd.send(tx_cmd, 0xC0A8010A, 9000); // 192.168.1.10

    std::cout << "\n2. Ticking time by 400ms (should trigger 3 retries, then a failure callback)\n";
    for (int i = 0; i < 4; ++i) {
        transport.m_time += 150;
        node.ack_tick();
    }

    std::cout << "\n3. Simulating receiving a packet back\n";
    uint8_t rx_packet[256];
    rx_packet[0] = lucp::MAGIC_0;
    rx_packet[1] = lucp::MAGIC_1;
    rx_packet[2] = 1; // msg_id
    rx_packet[3] = 42; // seq_id
    std::memcpy(&rx_packet[lucp::HEADER_SIZE], &tx_cmd, sizeof(MotorPayload));

    // Handled natively by the registered C++ object!
    node.process_packet(rx_packet, lucp::HEADER_SIZE + sizeof(MotorPayload), 0xC0A8010A, 9000);

    std::cout << "\n4. Verifying receive_incoming() helper\n";
    transport.queue_packet(rx_packet, lucp::HEADER_SIZE + sizeof(MotorPayload), 0xC0A8010A, 9001);
    transport.queue_packet(rx_packet, lucp::HEADER_SIZE + sizeof(MotorPayload), 0xC0A8010B, 9002);

    // This should process both queued packets
    node.receive_incoming();

    std::cout << "\n5. Flushing echo queue\n";
    node.flush_echo_queue();

    std::cout << "\nTest Complete.\n";
    return 0;
}
