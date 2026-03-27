#include "../include/lucp/node.hpp"
#include <iostream>
#include <vector>

// Mock Transport for testing
class MockTransport : public lucp::ITransport {
public:
    int send(const uint8_t* buf, uint16_t len, uint32_t ip, uint16_t port) override {
        std::cout << "MockTransport: Sending " << len << " bytes to " << ip << ":" << port << "\n";
        return len;
    }

    struct IncomingPacket {
        std::vector<uint8_t> data;
        uint32_t ip;
        uint16_t port;
    };
    std::vector<IncomingPacket> m_incoming;

    void queue_packet(const uint8_t* buf, uint16_t len, uint32_t ip, uint16_t port) {
        m_incoming.push_back({std::vector<uint8_t>(buf, buf + len), ip, port});
    }

    int receive(uint8_t* buf, uint16_t max_len, uint32_t& src_ip, uint16_t& src_port) override {
        if (m_incoming.empty()) return 0;
        auto packet = m_incoming.front();
        m_incoming.erase(m_incoming.begin());
        uint16_t to_copy = (packet.data.size() > max_len) ? max_len : (uint16_t)packet.data.size();
        std::memcpy(buf, packet.data.data(), to_copy);
        src_ip = packet.ip;
        src_port = packet.port;
        return to_copy;
    }

    uint32_t now_ms() override {
        return m_time;
    }

    void log_unknown(uint8_t msg_id, uint32_t /*src_ip*/, uint16_t /*src_port*/) override {
        std::cout << "MockTransport: Log Unknown msg_id=" << (int)msg_id << "\n";
    }

    void log_rejected(uint8_t msg_id, uint16_t received, uint16_t expected) override {
        std::cout << "MockTransport: Log Rejected msg_id=" << (int)msg_id 
                  << " (got " << received << ", expected " << expected << ")\n";
    }

    uint32_t m_time = 0;
};

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
        node.tick();
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

    std::cout << "\n4. Verifying process_incoming() helper\n";
    transport.queue_packet(rx_packet, lucp::HEADER_SIZE + sizeof(MotorPayload), 0xC0A8010A, 9001);
    transport.queue_packet(rx_packet, lucp::HEADER_SIZE + sizeof(MotorPayload), 0xC0A8010B, 9002);
    
    // This should process both queued packets
    node.process_incoming();

    std::cout << "\n5. Ticking to trigger Echo Queue drain\n";
    node.tick();

    std::cout << "\nTest Complete.\n";
    return 0;
}
