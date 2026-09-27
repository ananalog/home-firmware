#!/usr/bin/env python3
"""Prints manifest.json for a firmware image and checks its descriptors (same parsing as the server)."""
import hashlib
import json
import struct
import subprocess
import sys

APP_DESC_OFFSET = 32
HOME_DESC_OFFSET = 32 + 256


def parse(path):
    data = open(path, "rb").read()
    if data[0] != 0xE9:
        sys.exit(f"{path}: not an ESP application image")
    magic, = struct.unpack_from("<I", data, APP_DESC_OFFSET)
    if magic != 0xABCD5432:
        sys.exit(f"{path}: esp_app_desc_t not found")
    cstr = lambda off, n: data[off:off + n].split(b"\0")[0].decode()
    version = cstr(APP_DESC_OFFSET + 16, 32)
    project = cstr(APP_DESC_OFFSET + 48, 32)
    idf = cstr(APP_DESC_OFFSET + 112, 32)
    hmagic, model, hwmask, pmaj, pmin = struct.unpack_from("<IHHBB", data, HOME_DESC_OFFSET)
    if hmagic != 0x454D4F48:
        sys.exit(f"{path}: Home descriptor not found at offset {HOME_DESC_OFFSET} (HOME_IMAGE_DESC missing?)")
    return {
        "project": project, "version": version, "idf": idf, "model": model, "hwRevMask": hwmask,
        "protocol": f"{pmaj}.{pmin}", "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
    }


if __name__ == "__main__":
    info = parse(sys.argv[1])
    if len(sys.argv) > 3:
        info["device"] = sys.argv[2]
        try:
            info["commit"] = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
        except Exception:
            pass
    print(json.dumps(info, indent=2))
