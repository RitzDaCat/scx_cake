#!/bin/bash
# Build script for scx_cake

set -e

echo "=== Building scx_cake ==="

# Build release version
cargo build --release

echo ""
echo "=== Build complete ==="
echo "Binary: ./target/release/scx_cake"
echo ""
echo "Run with: sudo ./start.sh"
