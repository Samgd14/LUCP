# Lightweight UDP Command Protocol (LUCP) - Specification v0.2

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
- `payload`: exactly `IMessage::size()` bytes for that `msg_id`. Zero-length data messages are impossible by design: `size() == 0` is rejected at registration, and a header-only packet is reserved for ACKs.

### 4.3 Packet Types

- Data packet: header + payload.
- ACK packet: header only (`size == HEADER_SIZE`).

### 4.4 Error Codes

All `Node` and message APIs return `int`: `OK` (0) on success, negative codes on error.

| Code | Name | Meaning |
|------|------|---------|
| 0 | `OK` | Success |
| -1 | `ERR_INVALID_ID` | msg_id is 0, out of range, or not registered |
| -2 | `ERR_QUEUE_FULL` | ACK/pending or echo queue is full |
| -3 | `ERR_PAL_SEND` | Transport send returned an unexpected byte count |
| -4 | `ERR_BAD_ARG` | Null pointer or undersized packet argument |
| -5 | `ERR_NOT_IMPLEMENTED` | (Reserved) operation not implemented |
| -6 | `ERR_INVALID_PACKET` | Bad magic, wrong payload size, or ACK on a non-ack message |
| -7 | `ERR_PACKET_TOO_LARGE` | Packet exceeds HEADER_SIZE + MaxPayloadSize |
| -8 | `ERR_INVALID_SIZE` | Message size() is 0 or exceeds MaxPayloadSize at registration |
| -9 | `ERR_ALREADY_REGISTERED` | A message is already registered for this msg_id |
| -10 | `ERR_CANNOT_ECHO` | An echo ACK send failed during flush |
| -11 | `ERR_HANDLE_MISSING` | handle() was not overridden (default returned) |
| -12 | `ERR_ON_FAIL_MISSING` | on_fail() was not overridden (default returned) |
| -13 | `ERR_NO_PENDING` | An ACK had no matching pending record (spurious/duplicate ACK) |
| -14 | `ERR_NOT_REGISTERED` | TypedMessage::send called before registration |

### 4.5 Size Constraints
The maximum payload size is defined by the `Node` template parameter `MaxPayloadSize`. MaxPayloadSize is bounded to 508 (so a full packet is <= 512 bytes). Keep payloads well under this to minimize fragmentation and retransmission cost on constrained networks.

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

Optional receive-side ordering parameter:

```cpp
virtual bool reject_out_of_order() const { return true; }
```

When `true` (the default), the receiver drops out-of-order (older-than-latest) packets for this `msg_id` — latest-takes-precedence. When `false`, the receiver accepts out-of-order packets (e.g. timestamped data where arrival order is irrelevant). See §6.4.1 for the dedup contract when out-of-order is accepted.

Receive and failure hooks (should be implemented per-platform):

```cpp
virtual int handle(const uint8_t* payload, uint16_t size);  // default returns ERR_HANDLE_MISSING
virtual int on_fail();                                       // default returns ERR_ON_FAIL_MISSING
```

Sending is performed via `INode::send_raw(...)` (implemented by `Node`), not on `IMessage` directly. `TypedMessage<TPayload>` provides a typed `send()` wrapper that forwards to the node it was registered against.

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

1. Assigns the current sequence byte for that message ID and (on a successful send) increments it for the next send. A failed send does not consume a sequence number.
2. Builds and sends the packet.
3. Stores a pending ACK record in a bounded internal table.

If the send call fails, the pending record for that packet is cleared.

The sequence counter is per msg_id (global across all destinations), not per-peer. The ip:port fields in ACK matching prevent cross-peer confusion. If multiple peers send the same msg_id to one receiver, their seqs interleave and the per-msg_id dedup (§6.4.1) may drop a newer packet from one peer relative to another's last seen — acceptable for the typical host↔node topology (one peer per msg_id per direction).

Sequence wraparound: `seq_id` is a rolling `uint8_t`. With the default `MaxPendingAcks=4`, an in-flight unacked send can never share a seq with a newer in-flight send, so ACK misattribution is unreachable. Configuring a large `MaxPendingAcks` (approaching 256) with high reliable throughput to a single peer can, after a 256-send wrap, let an ACK clear the wrong (older) pending entry. Keep `MaxPendingAcks` modest for your throughput.

### 6.2 ACK Matching

An inbound header-only packet is treated as an ACK candidate.

It clears a pending record only when all fields match:

- `msg_id`
- `seq_id`
- source `ip`
- source `port`

ACKs for non-ack-required messages return `ERR_INVALID_PACKET`.

This requires the ACK echo to be sourced from the same IP:port the data was sent to. NAT or asymmetric routing that changes the source endpoint will break ACK matching silently.

An ACK that matches no pending record (spurious or duplicate ACK) returns `ERR_NO_PENDING`; the pending table is unchanged.

### 6.3 Retry and Exhaustion

`Node::tick()` drives retries using `ITransport::now_ms()` and each message's `retry_delay_ms()`.

- While retries remain: packet is resent and `retries_remaining` is decremented.
- When retries are exhausted: pending record is removed and `on_fail()` is called.

Retry delay is fixed (`retry_delay_ms()`); there is no exponential backoff. This is intentional for predictable behavior on constrained nodes.

If a retry's transport send fails, the retry is logged via `log_error` and `retries_remaining` is NOT decremented — the retry is attempted again on the next tick.

### 6.4 Echo ACK on Receive

For inbound data packets of ack-required messages:

1. Payload is validated.
2. `handle()` is called.
3. If `handle()` does not return an error, the original 4-byte header is queued for echo.
4. Echo headers are dispatched when `flush_echo_queue()` (or `process_all()`) is called.

If the echo queue is full, the packet is dropped before `handle()` is called.

### 6.4.1 Duplicate / Stale-Packet Suppression

The receiver tracks, per msg_id, the last handled sequence byte. For an inbound data packet it computes the wrap-aware distance from the last handled seq:

```
diff = ((last_seen - seq) & 0xFF)        // 0xFF sentinel when msg_id unseen
```

- `diff == 0` — exact retransmit of the latest handled seq: `handle()` is NOT called; the ACK echo is still sent so the sender stops retrying. (Unconditional — exact retransmits are always suppressed.)
- `diff` in `1..127` — older (behind the latest) within the half-window:
  - If `IMessage::reject_out_of_order()` returns `true` (the default): `handle()` is NOT called; the ACK echo is still sent (latest-takes-precedence).
  - If it returns `false`: `handle()` IS called and the ACK echo is sent; the last-seen seq is NOT advanced (out-of-order accepted).
- `diff` in `128..255` — newer (ahead / wrapped): `handle()` is called, the ACK echo is sent, and the last-seen seq is advanced.

Latest-takes-precedence is the default. It is the expected behavior for low-latency data streaming, where only the newest value matters. Set `reject_out_of_order()` to `false` for message types carrying timestamped data where every distinct packet is meaningful regardless of arrival order.

**Caveat when out-of-order is accepted:** the node suppresses only the exact retransmit of the most-recent seq. Retransmits of older (out-of-order) seqs MAY be handled more than once, because the single per-`msg_id` counter cannot distinguish an already-handled older seq from a fresh one. Applications receiving timestamped data should dedup by timestamp. (A per-`msg_id` handled-seq bitmap that would close this gap was rejected on RAM grounds — ~32 bytes per `msg_id`.)

The flag does not affect ACK behavior: dropped and handled out-of-order packets are both acknowledged so the sender stops retrying either way.

Dedup state is keyed per msg_id and is reset by both `reset()` and `reset_state()`.

---

## 7. Node Dispatch Behavior

`Node::process_packet(...)` behavior in the current implementation:

1. Drop packet if null or shorter than header.
2. Validate magic bytes.
3. Reject if size > HEADER_SIZE + MaxPayloadSize (ERR_PACKET_TOO_LARGE).
4. Validate `msg_id` (`1..MsgCount-1`) and lookup registered message.
5. Return an error code for invalid/unregistered messages.
6. If `size == HEADER_SIZE`, process as ACK path.
7. Otherwise require payload size exactly equal to `IMessage::size()`.
8. Duplicate/stale suppression (see §6.4.1): drop exact retransmits unconditionally; drop out-of-order-older packets only if `reject_out_of_order()` (default true); still echo ACK either way.
9. Call `handle(payload, payload_size)`.
10. For ack-required messages, queue echo ACK (unless queue full).

`handle()` return convention: `0` (OK) or any non-negative value = success (ACK is echoed); a negative value = failure (no ACK, the error is propagated). The default `handle()` returns `ERR_HANDLE_MISSING`.

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

`TypedMessage<TPayload>::send(...)` forwards to the `INode` instance set during registration via `INode::send_raw(...)`. Calling `send()` on an unregistered message returns `ERR_NOT_REGISTERED`. `INode` is the abstract interface (declared in `message.hpp`) that decouples messages from the `Node` template.

### 9.3 Runtime APIs

```cpp
int process_packet(const uint8_t *packet,
                   uint16_t size,
                   uint32_t source_ip,
                   uint16_t source_port);
void receive_incoming();    // Drains transport RX buffer (polling mode)
void ack_tick();            // Drives ACK retry and exhaustion callbacks
void flush_echo_queue();    // Dispatches queued outbound echo ACKs
void process_all();         // Convenience: receive_incoming + ack_tick + flush_echo_queue
void reset();        // Full wipe: clears the message registry, sequence counters, ACK tracking, echo queue, and dedup state.
void reset_state();  // State-only: clears sequence counters, ACK tracking, echo queue, and dedup state; PRESERVES the message registry.
```

`process_packet(...)` returns a LUCP error code (0 = OK, negative = error).

`receive_incoming()` polls `ITransport::receive()`. For interrupt/callback-driven transports,
call `process_packet()` directly and skip `receive_incoming()`.

`ack_tick()` and `flush_echo_queue()` must each be called regularly. Use `process_all()` for
simple main-loop polling where all three share the same cadence.

---

## 10. Implementation Notes

### 10.1 Memory Behavior

Core containers are statically sized via template parameters.

One notable stack allocation exists in `Node::send_raw(...)`, where a local packet buffer of `HEADER_SIZE + MaxPayloadSize` bytes is built before transport send. Select `MaxPayloadSize` with your platform stack budget in mind.

Each pending-ACK slot costs `HEADER_SIZE + MaxPayloadSize` bytes; total pending-ACK RAM is `MaxPendingAcks * (HEADER_SIZE + MaxPayloadSize)`. Budget accordingly on constrained MCUs.

### 10.2 Message IDs

The library reserves only ID `0` as invalid. Other ID meanings are application-defined.

### 10.3 Endianness

The LUCP header fields are single-byte values and are byte-order agnostic. Payload endianness should be kept consistent across peers; implement shared message structs and serialization logic accordingly.

On receive, the payload pointer is into a `uint8_t` buffer (alignment 1). Use `std::memcpy` to unpack payloads into typed structs; do not `reinterpret_cast` the pointer, which may be misaligned.

### 10.4 Scheduling

For reliable messaging and ACK echo behavior to work as intended:

- **`receive_incoming()`** should be called as fast as possible in a polling main loop, or driven by a receive-ready interrupt. Not needed for interrupt/callback-driven transports that call `process_packet()` directly.
- **`ack_tick()`** should be called at a predictable cadence (e.g., every 10 ms) so retry delays are accurate.
- **`flush_echo_queue()`** should be called regularly to dispatch pending echo ACKs with low latency.
- **`process_all()`** calls all three in the correct order and is sufficient for simple single-threaded main loops.

### 10.5 Versioning

The 4-byte header has no protocol version field; the magic bytes are the only format discriminator. Future breaking wire changes would require new magic values.

### 10.6 Reentrancy

`handle()` and `on_fail()` run inline on the calling thread. They must NOT call `send_raw()` or any other Node-mutating API (including via another message's `send()`). The Node is single-threaded; reentering a mutating API from within a callback during `ack_tick()` or `process_packet()` iteration is undefined behavior. If a callback needs to send, defer the send to after the current `process_all()`/`ack_tick()` call returns.

---

End of specification - LUCP v0.2