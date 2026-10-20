#!/usr/bin/env python3
"""Create a complete ESP32-S3 factory update package from real PlatformIO images.

Produces the exact Avolyn schema-v1 package and adjacent release manifest.
No firmware is compiled, flashed, uploaded or published by this script.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import re

FLASH_CAPACITY = 8 * 1024 * 1024
MAX_PACKAGE = 12 * 1024 * 1024
VERSION = re.compile(r"^(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)(?:-[A-Za-z0-9]+(?:[.-][A-Za-z0-9]+)*)?$")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def image_ok(data):
    return len(data) >= 24 and data[0] == 0xE9 and int.from_bytes(data[12:14], "little") == 9


def firmware_source_version(source):
    match = re.search(r'FIRMWARE_VERSION\[\]\s*=\s*"([^"]+)"', source.read_text(encoding="utf-8"))
    if not match:
        raise ValueError("Missing Avolyn firmware version in avolyn_headless.cpp.")
    return match.group(1)


def parse_partitions(data):
    if len(data) < 32 or data[:2] != b"\xaa\x50":
        raise ValueError("Invalid ESP32 partition table.")
    entries = []
    for pos in range(0, min(len(data), 0xC00), 32):
        entry = data[pos:pos + 32]
        if len(entry) < 32 or entry[:2] in (b"\xff\xff", b"\xeb\xeb"):
            break
        if entry[:2] != b"\xaa\x50":
            raise ValueError("Invalid ESP32 partition entry.")
        offset = int.from_bytes(entry[4:8], "little")
        size = int.from_bytes(entry[8:12], "little")
        if not size or offset < 0x9000 or offset % 4096 or size % 4096 or offset + size > FLASH_CAPACITY:
            raise ValueError("Invalid ESP32 partition location.")
        entries.append((entry[2], entry[3], offset, size))
    return entries


def build_package(build, source, version, output, boot_app0):
    if not VERSION.fullmatch(version) or len(version) > 64:
        raise ValueError("Invalid semantic firmware version.")
    actual = firmware_source_version(source)
    if version != actual:
        raise ValueError(f"Requested version {version} differs from compiled source version {actual}.")

    images = [(0x0, build / "bootloader.bin"),
              (0x8000, build / "partitions.bin"),
              (0x10000, build / "firmware.bin")]
    for _, file in images:
        if not file.is_file():
            raise ValueError(f"Missing required PlatformIO image: {file.name}")

    boot = images[0][1].read_bytes()
    partition = images[1][1].read_bytes()
    app = images[2][1].read_bytes()
    if not image_ok(boot) or not image_ok(app):
        raise ValueError("Bootloader and app must be ESP32-S3 executable images.")
    entries = parse_partitions(partition)
    app_part = next((p for p in entries if p[0] == 0 and p[1] in (0, 0x10)), None)
    if not app_part or app_part[2] != 0x10000 or len(app) > app_part[3]:
        raise ValueError("The application does not fit the factory slot at 0x10000.")
    ota = [p for p in entries if p[0] == 1 and p[1] == 0]
    if ota:
        if len(ota) != 1 or ota[0][2] != 0xE000 or ota[0][3] != 8192:
            raise ValueError("This OTA layout needs a verified, supported ota_data partition.")
        if not boot_app0 or not boot_app0.is_file() or boot_app0.stat().st_size != ota[0][3]:
            raise ValueError("Supply the matching 8192-byte boot_app0.bin for this OTA layout.")
        images.append((ota[0][2], boot_app0))
    elif boot_app0:
        raise ValueError("Do not add OTA boot data when this partition table has no ota_data slot.")

    images.sort()
    parts = []
    previous_end = 0
    for address, file in images:
        content = file.read_bytes()
        padded_end = address + ((len(content) + 4095) // 4096) * 4096
        if not content or address < previous_end or padded_end > FLASH_CAPACITY:
            raise ValueError("Firmware images overlap or exceed 8 MB of flash.")
        previous_end = padded_end
        parts.append({"address": address, "data": base64.b64encode(content).decode("ascii"), "sha256": sha256(content)})

    panel = {"id": "headless-wifi-test", "name": "Headless Wi-Fi and Secure Pairing Test", "width": 0, "height": 0}
    notes = "Headless development firmware only. Requires physical XIAO ESP32-S3. Factory installation can erase Wi-Fi and pairing state."
    package = {"schemaVersion": 1, "product": "avolyn-slate", "board": "xiao-esp32s3",
               "chip": "ESP32-S3", "version": version, "layout": "factory-s3-v1",
               "panel": panel, "notes": notes, "parts": parts}
    package_data = json.dumps(package, separators=(",", ":")).encode("utf-8")
    if len(package_data) > MAX_PACKAGE:
        raise ValueError("The packaged firmware exceeds Avolyn's 12 MB maximum.")
    release = {key: package[key] for key in ("schemaVersion", "product", "board", "chip", "version", "panel", "notes")}
    release.update({"packageUrl": output.name, "bytes": len(package_data), "sha256": sha256(package_data)})
    if output.exists() or Path(str(output) + ".release.json").exists():
        raise ValueError("Refusing to replace an existing package or manifest.")
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("xb") as f:
        f.write(package_data)
    with Path(str(output) + ".release.json").open("x", encoding="utf-8") as f:
        json.dump(release, f, indent=2)
        f.write("\n")
    return release


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--source", default="src/avolyn_headless.cpp", type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--boot-app0", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        release = build_package(args.build, args.source, args.version, args.output, args.boot_app0)
    except (ValueError, OSError) as exc:
        parser.error(str(exc))
    print(json.dumps({key: release[key] for key in ("version", "bytes", "sha256", "packageUrl")}, indent=2))


if __name__ == "__main__":
    main()
