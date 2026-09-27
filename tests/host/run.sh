#!/usr/bin/env bash
# Host-side tests: the CRC of the sensor driver (compiled without ESP-IDF).
set -euo pipefail
cd "$(dirname "$0")"
tmp=$(mktemp -d)
# acd1200.c needs FreeRTOS/I2C headers; the CRC function is extracted for the host build.
sed -n '/^uint8_t acd1200_crc8/,/^}/p' ../../components/acd1200/acd1200.c > "$tmp/crc.c"
sed -i '1i #include <stdint.h>' "$tmp/crc.c"
gcc -Wall -Werror -o "$tmp/t" test_host.c "$tmp/crc.c"
"$tmp/t"
