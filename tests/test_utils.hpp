#pragma once

#include <iostream>
#include <string>

#define ASSERT_TRUE(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "Assertion failed: (" << #condition << ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
            exit(1); \
        } \
    } while (0)

#define ASSERT_EQ(val1, val2) \
    do { \
        if ((val1) != (val2)) { \
            std::cerr << "Assertion failed: " << (val1) << " != " << (val2) << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            exit(1); \
        } \
    } while (0)

// Standard Mock transport
#include "../include/lucp/transport.hpp"
#include <vector>

struct SentPacket {
    std::vector<uint8_t> data;
    uint32_t ip;
    uint16_t port;
};

class MockTransport : public lucp::ITransport {
public:
    std::vector<SentPacket> sent_packets;
    uint32_t m_time = 0;
    int send_result = 0;
    
    int unknown_count = 0;
    int rejected_count = 0;

    int send(const uint8_t* buf, uint16_t len, uint32_t ip, uint16_t port) override {
        if (send_result < 0) return send_result;
        sent_packets.push_back({std::vector<uint8_t>(buf, buf + len), ip, port});
        return len;
    }

    struct IncomingPacket {
        std::vector<uint8_t> data;
        uint32_t ip;
        uint16_t port;
    };
    std::vector<IncomingPacket> incoming_packets;

    void queue_packet(const uint8_t* buf, uint16_t len, uint32_t ip, uint16_t port) {
        incoming_packets.push_back({std::vector<uint8_t>(buf, buf + len), ip, port});
    }

    int receive(uint8_t* buf, uint16_t max_len, uint32_t& src_ip, uint16_t& src_port) override {
        if (incoming_packets.empty()) return 0;
        auto packet = incoming_packets.front();
        incoming_packets.erase(incoming_packets.begin());
        
        uint16_t to_copy = static_cast<uint16_t>(packet.data.size());
        if (to_copy > max_len) to_copy = max_len;
        
        std::memcpy(buf, packet.data.data(), to_copy);
        src_ip = packet.ip;
        src_port = packet.port;
        return to_copy;
    }

    uint32_t now_ms() override {
        return m_time;
    }

    void log_unknown(uint8_t msg_id, uint32_t src_ip, uint16_t src_port) override {
        unknown_count++;
    }

    void log_rejected(uint8_t msg_id, uint16_t received, uint16_t expected) override {
        rejected_count++;
    }
};
