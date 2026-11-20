#!/bin/sh
# Builds libdlssg_sm86.so (needs gcc and the Vulkan headers) and runs the SHA-256 self-test.
set -e
cd "$(dirname "$0")"
mkdir -p build
gcc -O2 -Wall -DSHA256_TEST -o build/sha256_test src/sha256.c && build/sha256_test
nice -n 19 gcc -O2 -fPIC -shared -Wall -o libdlssg_sm86.so src/shim.c src/patches.c src/vk.c src/sha256.c -ldl
echo "built $(pwd)/libdlssg_sm86.so"
