# Lightweight UDP Control Protocol (LUCP) — Specification v0.2

---

## Table of Contents

1. [Overview](#1-overview)
2. [Design Principles](#2-design-principles)
3. [Network Topology](#3-network-topology)
4. [Wire Format](#4-wire-format)
5. [Message Schema System](#5-message-schema-system)
6. [Acknowledgment Mechanism](#6-acknowledgment-mechanism)
7. [Dispatch & Node Framework](#7-dispatch--node-framework)
8. [Transport Abstraction (ITransport)](#8-transport-abstraction-itransport)
9. [Shared C++ Core — Interface Reference](#9-shared-c-core--interface-reference)
10. [Performance Targets](#10-performance-targets)
11. [Implementation Notes](#11-implementation-notes)

---

## 1. Overview

LUCP is a binary UDP protocol designed for low-latency, near-real-time command and telemetry exchange between a host server (ARM/x86) and a fleet of resource-constrained embedded nodes (RP2040, ESP32, STM32, etc). It prioritizes minimal wire overhead, near-deterministic processing, and a heavily templated C++ implementation that compiles without modification on both sides.

This document defines version 0.2 of the wire format, the message registry object model, the acknowledgment mechanism, and the transport abstraction layer required to route communications contextually.

---

## 2. Design Principles

| Principle | Rationale |
|-----------|-----------|
| **Minimal header overhead** | Every byte matters on embedded MCUs |
| **Fixed-size messages** | Eliminates length parsing; enables O(1) validation |
| **Shared C++ core** | Single template codebase for server/node; totally object-oriented |
| **Echo-based ACK** | No separate validation message; receiver echoes the 4-byte header |
| **Per-message sequence tracking** | Avoids false ACK matches across concurrent streams |
| **Zero dynamic memory allocation** | Everything is rigidly bounded by compile-time templates on the node |
| **Little-endian wire format** | Consistent with modern target architecture alignments |

---

## 3. Network Topology

```
[ARM/x86 Server] <---> [Standard Ethernet Switch] <---> [MCU Node A]
       |                          |                  -> [MCU Node B]
  UDP Port 9000             Standard Switch          -> [MCU Node N]
  Static/DHCP IP           (No TSN required)            DHCP Assigned
```

- **Transport:** UDP/IPv4 over standard Ethernet
- **Default port:** 9000 (configurable)
- **Node addressing:** DHCP-assigned IPv4 by default; for permanent deployments use static IPs or DHCP reservations (see §3.1)
- **Multi-application scaling:** If an application requires more than 256 distinct message concepts, instantiate another UDP socket parsing via an entirely separated Node context on another port.

### 3.1 Address Assignment Strategy

Address stability is operationally important because the server tracks node endpoints by `(ip, port)`.

- **Development / lab:** Plain DHCP is acceptable.
- **Permanent deployments (recommended):**
    - Use **DHCP reservations** (preferred) keyed by node MAC address, or
    - Use **static IP** configuration on each node.
- **Server requirement:** Maintain persistent node identity mapping (`node_id -> ip, port, mac`) and log endpoint changes.

---

## 4. Wire Format

### 4.1 Header Structure

The header is exactly **4 bytes**, followed immediately by the message payload.

```
 0               1               2               3
 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   Magic[0]    |   Magic[1]    |  Message ID   |  Sequence ID  |
|     0xFA      |     0x51      |  (0x00–0xFF)  |  (0x00–0xFF)  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Payload (N bytes)                      |
|                 (length defined by message type)              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Offset | Size | Field | Value / Constraints |
|--------|------|-------|---------------------|
| 0-1 | 2 bytes | `magic` | Fixed: `0xFA` and `0x51` — protocol identifier and noise filter |
| 2 | 1 byte | `msg_id` | Message type identifier, `0x00–0xFF` |
| 3 | 1 byte | `seq_id` | Rolling sequence counter, wraps at 256 |
| 4-N | N bytes | `payload` | Fixed-length payload as encoded structurally |

### 4.2 Field Details

**`magic` (2 bytes)**
The two-byte magic sequence `0xFA` followed by `0x51` serves as a fast noise filter. A packet is discarded immediately if either byte does not match. Not guaranteed integrity; UDP checksum validates integrity.

**`msg_id` (1 byte)**
Identifies the message type up to 255 elements.

**`seq_id` (1 byte)**
A rolling counter incremented by the sender on each transmission of a given `msg_id`. The counter is maintained **per message type**, not globally. Wraps from `0xFF` to `0x00` without error.

**`payload` (variable, fixed per type)**
Raw binary payload. All multi-byte fields within the payload are encoded in **little-endian** byte order. The dispatch layer enforces size prior to passing data up to appropriate handler.

### 4.3 Total Packet Size

```
Total packet size = 4 (header) + payload_size (from registry class structure)
```

Practical upper boundaries:
- **Preferred payload size:** `<= 32 bytes`
- **Default upper bound:** `<= 256 bytes`
- **No-fragmentation MTU:** `<= 1468 bytes` payload

---

## 5. Message Schema System

### 5.1 Object Model

The message registry is constructed from class definitions mapped dynamically at runtime via `lucp::Node::register_message`.
Because this runs directly on the Node without `new` or `malloc`, the instantiations define all properties naturally via `virtual` properties.

```cpp
virtual uint8_t  id() const = 0;              // Identification byte
virtual uint16_t size() const = 0;            // Static payload sizing limits
virtual bool     ack_required() const = 0;    // Whether this requires Echoing
virtual uint8_t  max_retries() const;         // Try allowance bounds
virtual uint16_t retry_delay_ms() const;      // Try timer intervals
```

Memory limits are configured strictly via the `Node` initialization. Attempting to override the Node's max allowed size caps will result in compilation errors. Stack usage is pre-allocated based on the maximum message size and the number of messages that can be queued.

### 5.2 Assigned Message Types

Message type IDs are assigned sequentially starting at `0x01`. ID `0x00` is reserved.

Here is what an example message ID allocation table looks like:
| ID | Name | Payload Size | ACK Required | Description |
|----|------|-------------|--------------|-------------|
| `0x00` | *(reserved)* | — | — | Invalid; discard on receipt |
| `0x01` | `MOTOR_CMD` | 8 bytes | Yes | Torque/position setpoint |
| `0x02` | `SENSOR_DATA` | 16 bytes | No | Processed telemetry (application metrics) |
| `0x03` | `SYSTEM_STATUS` | 12 bytes | No | Health / generic data points |
| `0x04-FF` | *(available)* | — | — | Other bindings |

---

## 6. Acknowledgment Mechanism

### 6.1 Echo-Based ACK Design

When a message is transmitted with `ack_required` mapped internally as `true`, the receiver immediately responds by queuing a header-only packet to the sender, matching the incoming message's header.

Here is how the echo packet could be validated:
```cpp
echo.magic[0]   == 0xFA
echo.magic[1]   == 0x51
echo.msg_id     == original.msg_id
echo.seq_id     == original.seq_id
echo.size       == 4 (no payload)
```

### 6.2 Sequence Counter Behavior

Counters are maintained per message type. Wraparounds natively increment safely (8-bit truncations) with expected recovery.

> **Hard constraint (type bound):** `max_retries < 256`.

### 6.3 Sender Behavior

1. Class `IMessage` encapsulates object sending. Node assigns Seq_ID natively.
2. UDP Payload constructed natively.
3. If ack_required: Add item to the ACK pending pool statically. Set timestamp ticks.
4. Node::tick() will recursively fire retries or cleanup.
5. In exhaustion timeout scenario, virtual on_fail() callback handles uncoupling safely.

### 6.4 Receiver Behavior

1. Node processes inbound packets from `process_packet`.
2. Native checks discard packets without registered components gracefully.
3. Payload validations execute ensuring safe internal sizes mapping onto structures.
4. Calls Native `msg->handle()`. 
5. Inbound sequences triggering ACKs are buffered to bounded static arrays to push later safely via Node `tick()`.

---

## 7. Dispatch & Node Framework

### 7.1 Dispatch Constants

Core protocol constants are standardized strictly in `protocol.hpp`.
Return values enforce structural errors implicitly via constants (e.g. `ERR_NOT_IMPLEMENTED`).

### 7.2 Instantiating the Node

Node sizes bounds are totally enforced during instantiation.

```cpp
#include <lucp/node.hpp>

// Template parameters for static bounds:
// Node<MsgCount, EchoQueueDepth, MaxPendingAcks, MaxPayloadSize>
lucp::Node<32, 16, 4, 128> my_node(my_sys_transport);
```

### 7.3 Message Subclassing

The implementation shifts natively towards heavily typed `TypedMessage` implementations or bare `IMessage` definitions.
Override `id()`, `ack_required()`, and other parameters.

```cpp
struct MyStruct { uint32_t x; float f; };

class MyCommand : public lucp::TypedMessage<MyStruct> {
public:
    uint8_t id() const override { return 0xA1; }
    bool ack_required() const override { return true; }

    int handle(const uint8_t* payload, uint16_t size) override {
        MyStruct received;
        std::memcpy(&received, payload, sizeof(MyStruct));
        // Action logic
        return lucp::OK;
    }

    int on_fail() override {
        // Retry limit exhausted
        return lucp::OK;
    }
};
```

### 7.4 Registry Dispatching

Registrations happen via object inheritance injection. Pass the message reference inside your setup:

```cpp
MyCommand my_cmd_msg;
my_node.register_message(&my_cmd_msg);

// Send explicitly matching defined payload sizes natively!
MyStruct msg_payload = {10, 3.14f};
my_cmd_msg.send(msg_payload, dest_ip, 9000);
```

### 7.5 Handler Budget Restraints
Handlers execute precisely inline within `node.process_packet()`. Unregistered, oversized, or improperly sequenced payloads ignore handler propagation fully directly at the array boundary layer. No allocations trigger locally. The architecture remains absolutely clean.

---

## 8. Transport Abstraction (ITransport)

The hardware agnostic implementation isolates network communication purely across an implementation interface wrapper. C style `lucp_pal` files have been excised entirely. You must implement `ITransport`.

### 8.1 Implementing The Interface

```cpp
class MyTransport : public lucp::ITransport {
public:
    int send(const uint8_t* buf, uint16_t len, uint32_t ip, uint16_t port) override {
        // e.g. Arduino EthernetUDP implementation...
        Udp.beginPacket(IPAddress(ip), port);
        Udp.write(buf, len);
        return Udp.endPacket() ? len : lucp::ERR_PAL_SEND;
    }

    uint32_t now_ms() override {
        return millis(); // Standard timing tick logic
    }
    
    // Optional diagnostics
    void log_unknown(uint8_t msg_id, uint32_t src_ip, uint16_t src_port) override {
        Serial.printf("Unknown ID %02X\n", msg_id);
    }
};
```

### 8.2 Memory Constraints

| Platform | Core RAM Budget | Dynamic Allocation |
|----------|-----------------|--------------------|
| Embedded Node (Low Config) | Bound by Node templates (< 1 KB) | `new`/`malloc` **Not utilized** |
| Server Side OS | Heap Allocation Acceptable bounds | **Allowed internally across app** |

All elements map inside bounds. Array arrays natively compile and occupy strictly the minimum footprints.

---

## 9. Shared C++ Core — Interface Reference

### 9.1 IMessage Core 

Abstract components define structural metadata required mapping commands sequentially and robustly across dispatch layers:

```cpp
class IMessage {
public:
    virtual uint8_t  id() const = 0;
    virtual uint16_t size() const = 0;
    virtual bool     ack_required() const = 0;
    
    // Optional overrides mappings 
    virtual uint8_t  max_retries() const { return 0; }
    virtual uint16_t retry_delay_ms() const { return 0; }

    // Execute logic handling callbacks mapped directly natively inside context.
    virtual int handle(const uint8_t* payload, uint16_t size) { return lucp::ERR_NOT_IMPLEMENTED; }
    virtual int on_fail() { return lucp::ERR_NOT_IMPLEMENTED; }
    
    // Abstracted helper explicitly enforcing context inheritance 
    int send_raw(const uint8_t* payload, uint32_t dest_ip, uint16_t dest_port);
};
```

### 9.2 Node Object Methods

The Node handles structural mapping explicitly. 

`process_packet()` accepts raw array buffering, matches exact length boundaries against ID tables natively spanning sizes correctly, then pushes payloads directly onto `msg->handle()`. 

`tick()` cascades internal bounded timeout states, evaluates echo queuing array allocations iteratively clearing queues sequentially against specified configurations natively matching `ITransport->now_ms()`. 

Calling `tick()` continuously propagates ACKs and pending responses effectively efficiently.

```cpp
my_node.tick(); 
my_node.process_packet(rxBuffer, rxSize, sourceIP, sourcePort);
```

### 9.3 Endianness Expectations

All multi-byte fields logically map uniformly cross **little-endian** natively inline across structures. Ensure alignment specifications are robust when converting between architectural implementations directly manually structurally inline via struct overlays directly. Cortex-M0 targets prefer struct `memcpy()` usage mapping explicitly.

---

## 10. Performance Targets

### 10.1 Latency Budget (Target: < 10 ms application round-trip)

| Component | Target | Measurement |
|-----------|--------|-------------|
| SPI transfer (MCU → W5500) | ≤ 20 µs | GPIO toggle around SPI write |
| Network switch latency | ≤ 100 µs | Standard unmanaged switch |
| Protocol Node processing | < 1 ms | Overhead measurements across node layer |

---

## 11. Implementation Notes

### 11.1 Packed Struct Alignment on Cortex-M0/M0+

The RP2040 does not support unaligned accesses directly reliably mapping overlays structurally.
Always utilize `memcpy` when injecting memory allocations reliably inherently locally inline identically mapping variables precisely statically inline:

```cpp
// Correct mapping behavior
lucp::Header hdr;
std::memcpy(&hdr, packet, sizeof(hdr));
```

The Node intrinsically follows this process extracting sequence headers logically robustly.

### 11.2 ISR Discipline

Protocol evaluations must not map natively structurally dynamically inside asynchronous interrupt allocations. Process inbound packets caching safely out-bound contexts reliably parsing memory safely inside `loop` or RTOS Thread queues effectively securely mapping states.

### 11.3 Multi-Port Capabilities

Spin up a new `lucp::Node` instance alongside an alternative `ITransport` UDP bound socket to securely map independent configurations safely scaling endpoints sequentially effectively mapping bounds directly.

---

*End of specification — LUCP v0.2*
