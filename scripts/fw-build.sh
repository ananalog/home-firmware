#!/usr/bin/env bash
# Builds a device firmware in the pinned ESP-IDF Docker image.
#   scripts/fw-build.sh <device> [--release]
# Output: dist/<device>/<version>/ (bootloader, partition table, app image, manifest.json, elf)
set -euo pipefail
cd "$(dirname "$0")/.."
DEVICE=${1:?usage: fw-build.sh <device> [--release]}
IDF_IMAGE=${IDF_IMAGE:-espressif/idf:v5.4.2}
[ -d "devices/$DEVICE" ] || { echo "no devices/$DEVICE" >&2; exit 1; }

# Version: tag "<device>/vX.Y.Z" on HEAD, else version.txt + commit.
TAG=$(git describe --tags --exact-match --match "$DEVICE/v*" 2>/dev/null || true)
if [ -n "$TAG" ]; then VERSION=${TAG#"$DEVICE/v"}
else VERSION="$(head -1 "devices/$DEVICE/version.txt")-dev.$(git rev-parse --short HEAD 2>/dev/null || echo local)"; fi
[ "${2:-}" = "--release" ] && [ -z "$TAG" ] && { echo "release build needs a tag $DEVICE/vX.Y.Z on HEAD" >&2; exit 1; }

RUN="docker run --rm -e FW_VERSION=$VERSION -v $PWD:/project -w /project/devices/$DEVICE -u $(id -u):$(id -g) -e HOME=/tmp $IDF_IMAGE"
if command -v idf.py >/dev/null && [ -z "${USE_DOCKER:-}" ]; then
    RUN="env FW_VERSION=$VERSION"
    cd "devices/$DEVICE"
    idf.py build
    cd - >/dev/null
else
    $RUN idf.py build
fi

B=devices/$DEVICE/build
OUT=dist/$DEVICE/$VERSION
mkdir -p "$OUT"
cp "$B/bootloader/bootloader.bin" "$B/partition_table/partition-table.bin" "$OUT/"
[ -f "$B/ota_data_initial.bin" ] && cp "$B/ota_data_initial.bin" "$OUT/"
cp "$B/$DEVICE.bin" "$OUT/$DEVICE-$VERSION.bin"
cp "$B/$DEVICE.elf" "$OUT/$DEVICE-$VERSION.elf"
[ "$DEVICE" = board-probe ] || python3 tools/manifest.py "$OUT/$DEVICE-$VERSION.bin" "$DEVICE" "$VERSION" > "$OUT/manifest.json"
[ "$DEVICE" = board-probe ] || python3 tools/check_size.py "$B/$DEVICE.bin" partitions/4mb.csv
echo "$OUT"
