#!/usr/bin/env python3
"""Unit tests for Avolyn's offline factory package generator (no hardware)."""
import base64
import json
from pathlib import Path
import tempfile
import unittest

from avolyn_package_release import build_package, sha256
from build_avolyn_candidate import candidate, validate_source_contract


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
        magic[128:140] = b"0.2.0-dev\0\0\0"
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

    def test_stale_compiled_app_is_rejected(self):
        app = (self.build / "firmware.bin").read_bytes().replace(b"0.2.0-dev", b"0.1.9-dev")
        (self.build / "firmware.bin").write_bytes(app)
        with self.assertRaisesRegex(ValueError, "compiled app"):
            self.package(self.boot)
        self.assertFalse(self.output.exists())

    def test_declared_compatibility_matches_manifest_and_package(self):
        profile = Path(__file__).resolve().parents[1] / "avolyn-release-profile.json"
        manifest = build_package(self.build, self.source, "0.2.0-dev", self.output, self.boot, profile)
        expected = json.loads(profile.read_text())
        self.assertEqual(manifest["compatibility"], expected)
        self.assertEqual(json.loads(self.output.read_text())["compatibility"], expected)

    def test_invalid_compatibility_cannot_be_packaged(self):
        profile = self.root / "profile.json"
        original = json.loads((Path(__file__).resolve().parents[1] / "avolyn-release-profile.json").read_text())
        for field, value in [("minimumDesktopVersion", "latest"), ("presetSlots", True),
                             ("displayRefresh", "false"), ("protocols", {"presets": "v1"})]:
            raw = dict(original)
            raw[field] = value
            profile.write_text(json.dumps(raw))
            with self.subTest(field=field), self.assertRaises(ValueError):
                build_package(self.build, self.source, "0.2.0-dev", self.output, self.boot, profile)
            self.assertFalse(self.output.exists())

    def test_factory_without_ota_does_not_need_boot_data(self):
        self.partition(False)
        self.package()
        parsed = json.loads(self.output.read_text())
        self.assertEqual(len(parsed["parts"]), 3)

    def test_complete_candidate_includes_verified_files_and_source_information(self):
        profile = Path(__file__).resolve().parents[1] / "avolyn-release-profile.json"
        output = self.root / "candidate"
        report = candidate(self.build, self.source, profile, self.boot, output,
                           commit="a" * 40, repository="DominicD213/yoritechTMNLFork", run_id="123")
        self.assertEqual(report["version"], "0.2.0-dev")
        self.assertEqual(report["sourceCommit"], "a" * 40)
        self.assertEqual(report["compatibility"]["minimumDesktopVersion"], "0.7.1-dev.3")
        self.assertEqual(json.loads((output / "candidate-info.json").read_text()), report)
        checksums = (output / "SHA256SUMS.txt").read_text().splitlines()
        self.assertEqual(len(checksums), 2)
        for line in checksums:
            digest, file = line.split("  ")
            self.assertEqual(sha256((output / file).read_bytes()), digest)
        self.assertIn("physical display refresh is disabled", (output / "READ-ME.txt").read_text())

    def test_source_contract_drift_and_unsafe_candidate_identity_are_rejected(self):
        profile_file = Path(__file__).resolve().parents[1] / "avolyn-release-profile.json"
        profile = json.loads(profile_file.read_text())
        profile["protocols"]["presetsLan"] = "avolyn-presets-lan-v999"
        with self.assertRaisesRegex(ValueError, "differs"):
            validate_source_contract(Path(__file__).resolve().parents[1], profile)
        for options in [{"commit": "not-a-sha"}, {"repository": "../outside"},
                        {"run_id": "123\nversion=spoofed"}, {"version": "0.9.9"}, {"run_attempt": "0"},
                        {"run_attempt": "2\nversion=spoofed"}]:
            with self.subTest(options=options), self.assertRaises(ValueError):
                candidate(self.build, self.source, profile_file, self.boot, self.root / "invalid", **options)
        self.assertFalse((self.root / "invalid").exists())

    def test_rerun_candidates_keep_distinct_names_and_both_downloads(self):
        profile = Path(__file__).resolve().parents[1] / "avolyn-release-profile.json"
        reports = [candidate(self.build, self.source, profile, self.boot, self.root / f"attempt-{attempt}",
                             commit="a" * 40, repository="DominicD213/yoritechTMNLFork", run_id="123", run_attempt=str(attempt))
                   for attempt in [1, 2]]
        self.assertNotEqual(reports[0]["artifactName"], reports[1]["artifactName"])
        self.assertEqual(reports[1]["workflowAttempt"], 2)
        for attempt, report in enumerate(reports, 1):
            folder = self.root / f"attempt-{attempt}"
            self.assertTrue((folder / report["package"]["file"]).is_file())
            self.assertEqual(json.loads((folder / "candidate-info.json").read_text()), report)


if __name__ == "__main__":
    unittest.main()
