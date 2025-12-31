#!/bin/bash
# Build script for scx_cake

set -e

echo "=== Building scx_cake ==="

# Build release version with native CPU optimizations
# This enables all CPU-specific features (AVX-512, etc.) for maximum performance

echo "Select build type:"
echo "1) Standard (Max Performance, No Stats) - Default"
echo "2) Debug (Includes Performance Stats)"
read -p "Enter choice [1]: " choice
choice=${choice:-1}

if [ "$choice" = "2" ]; then
    echo "Building with Debug Stats..."
    RUSTFLAGS="-C target-cpu=native" cargo build --release --features debug_stats
else
    echo "Building Standard Release..."
    RUSTFLAGS="-C target-cpu=native" cargo build --release
fi

echo ""
echo "=== Build complete ==="
echo "Binary: ./target/release/scx_cake"
echo ""
echo "Run with: sudo ./start.sh"
