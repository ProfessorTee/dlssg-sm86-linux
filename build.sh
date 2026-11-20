#!/bin/sh
# Builds libdlssg_sm86.so. JOBS is unused (single TU set), nice keeps the desktop responsive.
set -e
cd "$(dirname "$0")"
gcc -O2 -Wall -DSHA256_TEST -o /tmp/sha256_test src/sha256.c && /tmp/sha256_test
nice -n 19 gcc -O2 -fPIC -shared -Wall -fvisibility=default -o libdlssg_sm86.so src/shim.c src/vk.c src/sha256.c -ldl
