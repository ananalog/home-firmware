#!/usr/bin/env python3
"""Fails when the app image uses more than 90% of an OTA slot."""
import os
import sys

image, table = sys.argv[1], sys.argv[2]
slot = None
for line in open(table):
    parts = [p.strip() for p in line.split("#")[0].split(",")]
    if len(parts) >= 5 and parts[2] == "ota_0":
        slot = int(parts[4], 0)
size = os.path.getsize(image)
pct = size * 100 // slot
print(f"app {size} bytes = {pct}% of the {slot}-byte OTA slot")
if pct > 90:
    sys.exit("image is too large for the OTA slot (>90%)")
