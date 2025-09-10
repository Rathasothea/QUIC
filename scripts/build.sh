#!/bin/bash

# Build script for MSQUIC project

set -e

cd "$(dirname "$0")/.."

echo "Building MSQUIC project..."

if [[ ! -d "build" ]]; then
    mkdir build
fi

cd build

cmake ..
make -j$(nproc)

echo ""
echo "Build completed successfully!"
echo ""
echo "Executables are in: $(pwd)/bin/"
echo ""
echo "To run:"
echo "  Server: ./bin/quic_server"
echo "  Client: ./bin/quic_client"