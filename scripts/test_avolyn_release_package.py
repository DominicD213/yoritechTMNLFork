#!/usr/bin/env python3
"""Unit tests for Avolyn's offline factory package generator (no hardware)."""
import base64
import json
from pathlib import Path
import tempfile
import unittest

from avolyn_package_release import build_package, sha256


class AvolynPackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="avolyn-pkg-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.build = self.root / "build"
        self.build.mkdir()
        self.source = self.root / "src.cpp"
        self.source.write_text('constexpr char FIRMWARE_VERSION[] = "0.2.0-dev";')
        self.output = self.root / "Avolyn-Slate-0.2.0-dev.avolyn-firmware"
        self.boot = self.root / "boot_app0.bin"
        self.boot.write_bytes(bytes(8192))
        magic = bytearray(4096)
        magic[0] = 0xE9
        magic[12:14] = (9).to_bytes(2, "little")
        (self.build / "bootloader.bin").write_bytes(magic)
        (self.build / "firmware.bin").write_bytes(magic)
        self.partition(True)

    def partition(self, ota):
        table = bytearray(b"\xff" * 4096)
        def put(index, kind, subtype, offset, size):
            pos = index * 32
            table[pos:pos+2] = b"\xaa\x50"
            table[pos+2] = kind
            table[pos+3] = subtype
            table[pos+4:pos+8] = offset.to_bytes(4, "little")
            table[pos+8:pos+12] = size.to_bytes(4, "little")
        put(0, 0, 0x10 if ota else 0, 0x10000, 0x100000)
        if ota:
            put(1, 1, 0, 0xE000, 8192)
        (self.build / "partitions.bin").write_bytes(table)

    def package(self, boot=None, version="0.2.0-dev"):
        return build_package(self.build, self.source, version, self.output, boot)

    def test_complete_ota_package_and_checksums(self):
        manifest = self.package(self.boot)
        raw = self.output.read_bytes()
        parsed = json.loads(raw)
        self.assertEqual(manifest["bytes"], len(raw))
        self.assertEqual(manifest["sha256"], sha256(raw))
        self.assertEqual(json.loads(Path(str(self.output) + ".release.json").read_text()), manifest)
        self.assertEqual([p["address"] for p in parsed["parts"]], [0, 0x8000, 0xE000, 0x10000])
        self.assertEqual(parsed["panel"]["width"], 0)
        for part in parsed["parts"]:
            self.assertEqual(sha256(base64.b64decode(part["data"])), part["sha256"])

    def test_missing_ota_image_rejected(self):
        with self.assertRaisesRegex(ValueError, "boot_app0"):
            self.package()
        self.assertFalse(self.output.exists())

    def test_source_version_mismatch_rejected(self):
        with self.assertRaisesRegex(ValueError, "differs"):
            self.package(self.boot, "0.2.1")
        self.assertFalse(self.output.exists())

    def test_existing_output_is_not_overwritten(self):
        self.package(self.boot)
        with self.assertRaisesRegex(ValueError, "Refusing"):
            self.package(self.boot)

    def test_bad_bootloader_rejected(self):
        (self.build / "bootloader.bin").write_bytes(b"garbage")
        with self.assertRaisesRegex(ValueError, "executable images"):
            self.package(self.boot)

    def test_factory_without_ota_does_not_need_boot_data(self):
        self.partition(False)
        self.package()
        parsed = json.loads(self.output.read_text())
        self.assertEqual(len(parsed["parts"]), 3)


if __name__ == "__main__":
    unittest.main()
