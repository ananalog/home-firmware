#!/usr/bin/env bash
# Serial monitor (USB-Serial-JTAG). scripts/fw-monitor.sh [port]
PORT=${1:-/dev/ttyACM0}
if command -v idf.py >/dev/null; then exec idf.py -p "$PORT" monitor --no-reset
elif python3 -c "import serial" 2>/dev/null; then exec python3 -m serial.tools.miniterm --raw "$PORT" 115200
else exec docker run --rm -it --device "$PORT" espressif/idf:v5.4.2 python -m serial.tools.miniterm --raw "$PORT" 115200; fi
