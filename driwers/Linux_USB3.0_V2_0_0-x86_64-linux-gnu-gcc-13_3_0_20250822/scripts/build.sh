#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd)

cd "$ROOT_DIR"

mkdir -p bin

echo "Building bin/plug617_stream_probe..."
gcc src/plug617_stream_probe.c \
  -o bin/plug617_stream_probe \
  -I sdk/include \
  -L sdk/lib \
  -lGuideUSBCamera \
  -lpthread \
  -Wl,-rpath,'$ORIGIN/../sdk/lib'

echo
echo "ldd bin/plug617_stream_probe:"
ldd bin/plug617_stream_probe || true

if ldd bin/plug617_stream_probe 2>/dev/null | grep -q "libGuideUSBCamera.so.*not found"; then
  echo
  echo "libGuideUSBCamera.so was not found by the dynamic linker."
  echo "Try:"
  echo "  export LD_LIBRARY_PATH=\$PWD/sdk/lib:\$LD_LIBRARY_PATH"
fi

echo
echo "Build complete: $ROOT_DIR/bin/plug617_stream_probe"
