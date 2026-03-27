# LUCP C++ Implementation Guide

The C++ port of the LUCP (Lightweight UDP Command Protocol) library provides a low-latency, object-oriented, and strictly allocation-free command architecture. It is designed to cleanly separate platform-specific hardware logic from core protocol mechanisms while being lightweight enough to run on embedded systems.

## 1. System Overview

The C++ architecture depends on three main pillars:
1. **`ITransport`**: An abstract interface that implements basic UDP capabilities and system time. This is the only place platform-specific integrations exist.
2. **`IMessage` / `TypedMessage`**: Abstract base classes encapsulating the definition (ID, Size, Reliability constraints) and handler behavior for individual commands.
3. **`Node<...>`**: The core protocol engine. It is highly templated, guaranteeing all internal buffer and state sizes are allocated either globally or strictly bounded on the stack at compile time, eliminating the need for `new` or `malloc`. It cleanly delegates protocol tasks to single-responsibility internal modules (`internal::EchoQueue` and `internal::AckManager`).

## 2. Implementing the Transport Layer (`ITransport`)

To run LUCP on a new platform (FreeRTOS, Arduino, ESP-IDF, Linux, Windows), implement a subclass of `lucp::ITransport`.

```cpp
#include <lucp/transport.hpp>
#include <iostream>

class MyPlatformTransport : public lucp::ITransport {
public:
    // Sends a UDP packet to the specified IP and port
    int send(const uint8_t* buf, uint16_t len, uint32_t ip, uint16_t port) override {
        // Implement native socket/UDP send logic here.
        // Example for Arduino Networking:
        // udp.beginPacket(ip, port);
        // udp.write(buf, len);
        // udp.endPacket();
        return len; // Return number of bytes successfully sent, or < 0 for error.
    }

    // Get current monotonic time in milliseconds
    uint32_t now_ms() override {
        // E.g., for Arduino: return millis();
        // E.g., for POSIX: Use clock_gettime(CLOCK_MONOTONIC, ...)
        return 0; 
    }

    // (Optional) Override debugging and logging callbacks natively
    void log_debug(const char* fmt, ...) override {
        // Print safely to standard out or serial
    }
    
    void log_unknown(uint8_t msg_id, uint32_t src_ip, uint16_t src_port) override {
        // ...
    }
};
```

## 3. Creating Custom Messages (`TypedMessage`)

The protocol replaces traditional C-style function pointers and switch cases with dedicated Command objects. Subclass `lucp::TypedMessage<TPayload>` if your payload maps to a C/C++ struct, or `lucp::IMessage` if dealing with dynamic byte streams or zero payloads.

```cpp
#include <lucp/message.hpp>

// 1. Define the exact byte layout of your payload
#pragma pack(push, 1)
struct TelemetryPayload {
    float temperature;
    float humidity;
};
#pragma pack(pop)

// 2. Derive a new Message handler class
class TelemetryMessage : public lucp::TypedMessage<TelemetryPayload> {
public:
    // Protocol Definitions
    uint8_t id() const override { return 0x10; }             // LUCP protocol Message ID
    bool ack_required() const override { return true; }      // Does this need guaranteed delivery?
    uint8_t max_retries() const override { return 3; }       // Retries if ACK fails
    uint16_t retry_delay_ms() const override { return 500; } // Wait between retries

    // Receiver Handler (Invoked when valid packet matching ID is parsed)
    int handle(const uint8_t* payload, uint16_t size) override {
        // size validation is mostly handled by node, but good practice to double check inside handler
        if (size != sizeof(TelemetryPayload)) return lucp::ERR_BAD_ARG;
        
        const auto* data = reinterpret_cast<const TelemetryPayload*>(payload);
        
        // Handle incoming data
        // ... update internal state ...

        // Returning 0 (OK) signals the Node to transmit an ACK if this message requires one
        return 0; 
    }

    // Optional: Fail Handler when retries are exhausted
    int on_fail() override {
        // The node exhausted all retries to send this message
        return 0;
    }
};
```

## 4. Bootstrapping and Running the Node

Once you have defined your `ITransport` and instantiated your message objects globally, configure and drive the main `Node`.

```cpp
#include <lucp/node.hpp>

// 1. Global Instances
MyPlatformTransport transport;

// Configure Node parameters via templates. By default, it allocates room for 256
// message types, 16 queued echo ACKs, 4 max pending ACKs, and a 256 byte max payload limit.
// Tune these heavily for embedded targets to reduce static RAM utilization.
lucp::Node<256, 16, 4, 128> node(transport);

TelemetryMessage telemetryMsg;

void setup() {
    // 2. Register application messages
    node.register_message(&telemetryMsg);
}

void loop() {
    // 3. Receive packets from hardware (non-blocking)
    uint8_t rx_buffer[128];
    uint32_t src_ip = 0;
    uint16_t src_port = 0;
    
    // User implement read_udp to check for packets
    uint16_t rx_size = read_udp(rx_buffer, &src_ip, &src_port); 
    
    if (rx_size > 0) {
        // Dispatch directly into the LUCP Node core
        node.process_packet(rx_buffer, rx_size, src_ip, src_port);
    }
    
    // 4. Tick the node regularly (Processes retries and flushes echo/ack queues)
    node.tick();
}

// 5. Sending Messages strongly typed! (No manual byte packing required)
void send_sensor_data() {
    TelemetryPayload data = { 24.5f, 55.0f };
    
    // send() is automatically bound into `telemetryMsg` through register_message()
    telemetryMsg.send(data, TARGET_IP, TARGET_PORT);
}
```

## Considerations for Embedded Environments

The architecture's `Node<MsgCount, EchoQueueDepth, MaxPendingAcks, MaxPayloadSize>` relies on compile-time templates to size its internal states (like `internal::EchoQueue` and `internal::AckManager`), strictly maintaining zero dynamic allocation.
 
* **`MaxPayloadSize`**: Impacts stack usage extremely heavily during `send_raw()` and the size footprint of the `internal::AckManager`. On an RP2040 or ESP32, you don't want a `MaxPayloadSize` that exceeds your RTOS task stack threshold, as the node will try to stack-allocate a buffer of `HEADER_SIZE + MaxPayloadSize` temporarily right before giving it to the transport layer. It also defines the max sizing for retries stored in the `AckManager`.
* **`MsgCount`**: Represents the largest allowed contiguous Message ID. If your maximum ID is `0x20` (32), limit `MsgCount = 33` to prevent wasting pointer array RAM for unused command indices.
* **`EchoQueueDepth`** & **`MaxPendingAcks`**: Determine the exact length of the static arrays allocated inside `internal::EchoQueue` and `internal::AckManager` respectively, impacting the global memory size of your instantiated Node. Keep them lean (e.g., depths of 4 to 16) for typical sensor and telemetry projects.
