# LUCP C++ Reference Implementation v0.2

Lightweight UDP Command Protocol (LUCP) is a binary UDP protocol designed for low-latency, near-real-time command and telemetry exchange between hosts and resource-constrained embedded nodes.

This repository contains the C++ implementation of the protocol.

## Documentation

For a comprehensive explanation of the library's architecture and usage, please see:

- [IMPLEMENTATION_GUIDE.md](IMPLEMENTATION_GUIDE.md): Guide to creating your `ITransport` PAL, defining `TypedMessage` classes, and bootstrapping the `Node`.
- [SPEC.md](SPEC.md): Full breakdown of the LUCP binary wire format, message registry semantics, and acknowledgment mechanism.

## Workspace Structure

- `include/lucp/`: Core header-only C++ templates
  - `node.hpp`: The core LUCP Protocol Node.
  - `message.hpp`: The base class for all messages.
  - `transport.hpp`: The transport layer interface.
  - `protocol.hpp`: The protocol layer interface.
- `tests/`: Unit and integration tests.

## Running Tests

CMake is used for building and executing the unit tests.

```powershell
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

```bash
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```