# Lightweight UDP Control Protocol (LUCP) - Specification v0.2

---

## Table of Contents

1. [Overview](#1-overview)
2. [Design Principles](#2-design-principles)
3. [Network Topology](#3-network-topology)
4. [Wire Format](#4-wire-format)
5. [Message Model](#5-message-model)
6. [Acknowledgment Mechanism](#6-acknowledgment-mechanism)
7. [Node Dispatch Behavior](#7-node-dispatch-behavior)
8. [Transport Abstraction (`ITransport`)](#8-transport-abstraction-itransport)
9. [C++ Interface Reference](#9-c-interface-reference)
10. [Implementation Notes](#10-implementation-notes)

---

## 1. Overview

LUCP is a compact binary UDP protocol for low-latency command and telemetry traffic between a host and embedded nodes.

This repository provides a shared C++ core that runs on both sides. The protocol layer is header-only, uses fixed compile-time bounds, and avoids dynamic allocation in core data paths.

Version `0.2` in this document matches the implementation under `include/lucp/`.

---

## 2. Design Principles

| Principle | Rationale |
|-----------|----------------|
| Minimal header overhead | Keep packet overhead low on constrained systems |
| Fixed payload size per message | Fast validation and simple parsing |
| Shared C++ message definitions | Keep host and node message contracts identical |
| Echo-based ACK | Reuse the header as ACK instead of defining a separate ACK packet type |
| Per-message sequence counters | Track in-flight reliable sends per message ID |
| Statically bound | Core queues and pending ACK state are fixed at compile time |

---

## 3. Network Topology

Typical deployment is UDP over IPv4 on standard Ethernet.

- Transport: UDP/IPv4
- Default port: `9000` (application configurable)
- Addressing: static IPs, DHCP reservations, or DHCP in development

LUCP does not mandate a single global discovery or identity system. Endpoint mapping is an application concern.

---

## 4. Wire Format

### 4.1 Header Layout

Each packet starts with a 4-byte header, which is then followed by the payload:

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

Constants in code:

- `MAGIC_0 = 0xFA`
- `MAGIC_1 = 0x51`
- `HEADER_SIZE = 4`

### 4.2 Field Details

- `magic`: fast protocol filter; packets with wrong magic bytes are dropped.
- `msg_id`: message identifier (`uint8_t`). Value `0` is invalid in current implementation.
- `seq_id`: rolling sequence byte assigned by sender per `msg_id`.
- `payload`: exactly `IMessage::size()` bytes for that `msg_id`.

### 4.3 Packet Types

- Data packet: header + payload.
- ACK packet: header only (`size == HEADER_SIZE`).

### 4.4 Size Constraints
The maximum payload size is defined by the `Node` template parameter `MaxPayloadSize`. The total packet size must fit within typical UDP limits (e.g., 512 bytes or less) to avoid fragmentation. It is recommended to minimize payload sizes for better performance and reliability on constrained networks and nodes.

---

## 5. Message Model

LUCP does not define built-in application message schemas. Applications provide message classes derived from `IMessage`.

Required message metadata (should be implemented project-wide):

```cpp
virtual uint8_t  id() const = 0;
virtual uint16_t size() const = 0;
virtual bool     ack_required() const = 0;
```

Optional reliability parameters (only relevant if `ack_required()` returns `true`):

```cpp
virtual uint8_t  max_retries() const { return 0; }
virtual uint16_t retry_delay_ms() const { return 0; }
```

Receive and failure hooks (should be implemented per-platform):

```cpp
virtual int handle(const uint8_t* payload, uint16_t size);
virtual int on_fail();
virtual int send_raw(const uint8_t* payload, uint16_t size, uint32_t dest_ip, uint16_t dest_port);
```

Important implementation rules:

- `id() == 0` is rejected at registration time.
- `size() == 0` is rejected at registration time.
- `size()` must be `<= MaxPayloadSize` of the `Node` template instance.
- Messages are registered by ID into a fixed array (`Node::register_message`).

The helper `TypedMessage<TPayload>` automatically implements `size()` as `sizeof(TPayload)` and provides a typed `send()` wrapper.

---

## 6. Acknowledgment Mechanism

### 6.1 Echo-based ACK

When a registered message has `ack_required()` return `true`, `Node::send_raw(...)`:

1. Assigns and increments the sequence byte for that message ID.
2. Builds and sends the packet.
3. Stores a pending ACK record in a bounded internal table.

If the send call fails, the pending record for that packet is cleared.

### 6.2 ACK Matching

An inbound header-only packet is treated as an ACK candidate.

It clears a pending record only when all fields match:

- `msg_id`
- `seq_id`
- source `ip`
- source `port`

ACKs for non-ack-required messages return `ERR_INVALID_PACKET`.

### 6.3 Retry and Exhaustion

`Node::tick()` drives retries using `ITransport::now_ms()` and each message's `retry_delay_ms()`.

- While retries remain: packet is resent and `retries_remaining` is decremented.
- When retries are exhausted: pending record is removed and `on_fail()` is called.

### 6.4 Echo ACK on Receive

For inbound data packets of ack-required messages:

1. Payload is validated.
2. `handle()` is called.
3. If `handle()` does not return `ERR_NOT_IMPLEMENTED`, the original 4-byte header is queued for echo.
4. Echo headers are dispatched when `flush_echo_queue()` (or `process_all()`) is called.

If the echo queue is full, the packet is dropped before `handle()` is called.

---

## 7. Node Dispatch Behavior

`Node::process_packet(...)` behavior in the current implementation:

1. Drop packet if null or shorter than header.
2. Validate magic bytes.
3. Validate `msg_id` (`1..MsgCount-1`) and lookup registered message.
4. Return an error code for invalid/unregistered messages.
5. If `size == HEADER_SIZE`, process as ACK path.
6. Otherwise require payload size exactly equal to `IMessage::size()`.
7. Call `handle(payload, payload_size)`.
8. For ack-required messages, queue echo ACK (unless queue full).

`Node::process_packet(...)` does not emit transport diagnostics hooks directly.
It only returns LUCP error codes.

`Node::receive_incoming()` is the boundary that logs protocol failures by invoking
`ITransport::log_error(error_code, src_ip, src_port)` when
`process_packet(...)` returns a non-zero error.

---

## 8. Transport Abstraction (`ITransport`)

Applications provide platform networking and time through `ITransport`:

```cpp
virtual int      send(const uint8_t* buf, uint16_t len, uint32_t ip, uint16_t port) = 0;
virtual uint32_t now_ms() = 0;
```

Optional polling-mode receive hook (default returns 0; interrupt-driven transports may skip this):

```cpp
virtual int receive(uint8_t* buf, uint16_t max_len, uint32_t& src_ip, uint16_t& src_port);
```

Optional diagnostics hooks:

```cpp
virtual void log_error(int error_code, uint32_t src_ip, uint16_t src_port);
```

The protocol core does not enforce byte order conversion of IP values; applications should keep sender/receiver conventions consistent.

---

## 9. C++ Interface Reference

### 9.1 `Node` Template

```cpp
template <size_t MsgCount      = 256,
          size_t EchoQueueDepth = 16,
          size_t MaxPendingAcks = 4,
          size_t MaxPayloadSize = 256,
          size_t MaxRecvBurst   = 64>
class Node;
```

- `MsgCount`: size of message registry and sequence table.
- `EchoQueueDepth`: maximum queued outbound ACK echoes.
- `MaxPendingAcks`: maximum concurrent reliable transmissions awaiting ACK.
- `MaxPayloadSize`: maximum payload size accepted and transmitted by this node.
- `MaxRecvBurst`: maximum packets drained per `receive_incoming()` call (prevents starvation).

### 9.2 Registration and Send APIs

```cpp
int register_message(IMessage* msg);
int send_raw(uint8_t msg_id,
             const uint8_t* payload,
             uint16_t payload_size,
             uint32_t dest_ip,
             uint16_t dest_port);
```

`IMessage::send_raw(...)` forwards to the node instance set during registration.

### 9.3 Runtime APIs

```cpp
void process_packet(const uint8_t* packet,
                    uint16_t size,
                    uint32_t source_ip,
                    uint16_t source_port);
void receive_incoming();    // Drains transport RX buffer (polling mode)
void ack_tick();            // Drives ACK retry and exhaustion callbacks
void flush_echo_queue();    // Dispatches queued outbound echo ACKs
void process_all();         // Convenience: receive_incoming + ack_tick + flush_echo_queue
void reset();
```

`receive_incoming()` polls `ITransport::receive()`. For interrupt/callback-driven transports,
call `process_packet()` directly and skip `receive_incoming()`.

`ack_tick()` and `flush_echo_queue()` must each be called regularly. Use `process_all()` for
simple main-loop polling where all three share the same cadence.

---

## 10. Implementation Notes

### 10.1 Memory Behavior

Core containers are statically sized via template parameters.

One notable stack allocation exists in `Node::send_raw(...)`, where a local packet buffer of `HEADER_SIZE + MaxPayloadSize` bytes is built before transport send. Select `MaxPayloadSize` with your platform stack budget in mind.

### 10.2 Message IDs

The library reserves only ID `0` as invalid. Other ID meanings are application-defined.

### 10.3 Endianness

The LUCP header fields are single-byte values and are byte-order agnostic. Payload endianness should be kept consistent across peers; implement shared message structs and serialization logic accordingly.

### 10.4 Scheduling

For reliable messaging and ACK echo behavior to work as intended:

- **`receive_incoming()`** should be called as fast as possible in a polling main loop, or driven by a receive-ready interrupt. Not needed for interrupt/callback-driven transports that call `process_packet()` directly.
- **`ack_tick()`** should be called at a predictable cadence (e.g., every 10 ms) so retry delays are accurate.
- **`flush_echo_queue()`** should be called regularly to dispatch pending echo ACKs with low latency.
- **`process_all()`** calls all three in the correct order and is sufficient for simple single-threaded main loops.

---

End of specification - LUCP v0.2