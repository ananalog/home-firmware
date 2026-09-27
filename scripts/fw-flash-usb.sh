#!/usr/bin/env bash
# First ("factory") flashing over USB.
#   scripts/fw-flash-usb.sh <device> [--port /dev/ttyACM0] [--version X] [--app-only]
# Full mode erases the chip and writes: bootloader, partition table, otadata, the app into the
# FACTORY slot (0x20000) — the image the device returns to on "factory firmware" reset.
# --app-only writes just the app into the factory slot (settings are kept).
# Needs esptool (pip install esptool) or Docker with access to the serial port.
set -euo pipefail
cd "$(dirname "$0")/.."
DEVICE=${1:?usage: fw-flash-usb.sh <device> [--port P] [--version V] [--app-only]}
shift
PORT=/dev/ttyACM0
VERSION=""
APP_ONLY=0
while [ $# -gt 0 ]; do
    case "$1" in
        --port) PORT=$2; shift ;;
        --version) VERSION=$2; shift ;;
        --app-only) APP_ONLY=1 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
[ -n "$VERSION" ] || VERSION=$(ls -t "dist/$DEVICE" | head -1)
DIR=dist/$DEVICE/$VERSION
APP=$(ls "$DIR"/"$DEVICE"-*.bin | head -1)
[ -f "$APP" ] || { echo "no build in $DIR — run scripts/fw-build.sh $DEVICE" >&2; exit 1; }

if command -v esptool.py >/dev/null; then ESPTOOL="esptool.py"
elif command -v esptool >/dev/null; then ESPTOOL="esptool"
else ESPTOOL="docker run --rm -i --device $PORT -v $PWD:/project -w /project espressif/idf:v5.4.2 esptool.py"; fi

COMMON="--chip esp32c3 --port $PORT --baud 921600"
if [ "$APP_ONLY" = 1 ]; then
    $ESPTOOL $COMMON write_flash 0x20000 "$APP"
else
    $ESPTOOL $COMMON erase_flash
    $ESPTOOL $COMMON write_flash --flash_size 4MB \
        0x0 "$DIR/bootloader.bin" \
        0x8000 "$DIR/partition-table.bin" \
        0xF000 "$DIR/ota_data_initial.bin" \
        0x20000 "$APP"
fi
echo "flashed $APP (factory slot). Device id = MAC of the chip (see the monitor or the Android app)."
