<<<<<<< HEAD
# New MSQUIC Project

A simple QUIC client-server application using Microsoft's MSQUIC library.

## Quick Start

### Build
```bash
# Create build directory
mkdir build && cd build

# Configure and build
cmake ..
make

# Or use ninja if available
cmake .. -GNinja
ninja
```

### Run

#### Server (Terminal 1):
```bash
cd build/bin
./quic_server
```

#### Client (Terminal 2):
```bash
cd build/bin
./quic_client
```

## Usage

1. Start the server first - it will listen on port 4567
2. Start the client - it will connect to localhost:4567
3. Type messages in the client to send to the server
4. The server will echo back your messages with "Server Echo: " prefix
5. Type 'quit' or 'exit' to stop the client
6. Press Ctrl+C to stop the server

## Dependencies

- MsQuic library
- CMake 3.16+
- C11 compiler (GCC, Clang, MSVC)

## File Structure

```
project/
├── src/
│   ├── server.c     # QUIC server implementation
│   └── client.c     # QUIC client implementation
├── build/           # Build directory
├── docs/            # Documentation
└── CMakeLists.txt   # Build configuration
```
=======
# QUIC
>>>>>>> c9c81831bad22f74cc4948c4fd8525fcd5d85b6a
