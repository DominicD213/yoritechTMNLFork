#!/usr/bin/env python3
"""Build a reviewable candidate artifact from already compiled images; never flash/publish."""
import argparse
import json
import os
from pathlib import Path
import re

from avolyn_package_release import build_package, compatibility_profile, firmware_source_version, sha256, VERSION

ROOT = Path(__file__).resolve().parents[1]


def validate_source_contract(root, profile):
    constants = {
        "pairing": ("avolyn_pairing_crypto.h", "PROTOCOL"),
        "frame": ("avolyn_frame_usb.h", "FRAME_PROTOCOL"),
        "presets": ("avolyn_frame_usb.h", "PRESET_PROTOCOL"),
        "presetsLan": ("avolyn_preset_lan.h", "PRESET_LAN_PROTOCOL"),
    }
    for name, (file, symbol) in constants.items():
        source = (root / "include" / file).read_text(encoding="utf-8")
        match = re.search(r'\b' + symbol + r'\[\]\s*=\s*"([^"]+)"', source)
        if not match or match.group(1) != profile["protocols"][name]:
            raise ValueError(f"Release profile differs from firmware {name} protocol.")
    slots = (root / "include" / "avolyn_preset_store.h").read_text(encoding="utf-8")
    match = re.search(r"\bPRESET_SLOTS\s*=\s*(\d+)", slots)
    if not match or int(match.group(1)) != profile["presetSlots"] or profile["displayRefresh"] is not False:
        raise ValueError("Release profile does not match the headless preset target.")


def candidate(build, source, profile_file, boot, output_dir, version="", commit="", repository="", run_id=""):
    actual = firmware_source_version(source)
    if not VERSION.fullmatch(actual) or len(actual) > 64 or (version and version != actual):
        raise ValueError("Requested firmware version differs from the checked-out source.")
    profile = compatibility_profile(profile_file)
    validate_source_contract(ROOT, profile)
    if commit and not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("Invalid source commit.")
    if repository and not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*/[A-Za-z0-9][A-Za-z0-9_.-]*", repository):
        raise ValueError("Invalid source repository.")
    if run_id and not re.fullmatch(r"[0-9]+", run_id):
        raise ValueError("Invalid workflow run ID.")
    output = output_dir / f"Avolyn-Slate-{actual}.avolyn-firmware"
    release = build_package(build, source, actual, output, boot, profile_file)
    manifest = Path(str(output) + ".release.json")
    report = {"schemaVersion": 1, "version": actual, "environment": "avolyn_headless",
              "sourceCommit": commit or None, "sourceRepository": repository or None,
              "workflowRun": f"https://github.com/{repository}/actions/runs/{run_id}" if repository and run_id else None,
              "compatibility": profile, "package": {"file": output.name, "bytes": release["bytes"], "sha256": release["sha256"]}}
    # Read back the exact files that will be retained in the artifact.
    if json.loads(manifest.read_text()) != release or sha256(output.read_bytes()) != release["sha256"]:
        raise ValueError("Candidate read-back verification failed.")
    (output_dir / "candidate-info.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    (output_dir / "SHA256SUMS.txt").write_text(
        "".join(f"{sha256(file.read_bytes())}  {file.name}\n" for file in [output, manifest]), encoding="utf-8")
    (output_dir / "READ-ME.txt").write_text(
        f"Avolyn Slate firmware {actual}\nRequires desktop {profile['minimumDesktopVersion']} or newer with the declared protocols.\n"
        "Headless development candidate: physical display refresh is disabled.\n"
        "Extract the artifact ZIP. Use the complete .avolyn-firmware file in the updated desktop app.\n"
        "The .release.json and SHA256SUMS.txt files describe and verify that package.\n"
        "Review board, firmware and app compatibility before approving installation. Building this artifact does not install firmware.\n",
        encoding="utf-8")
    return report


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--source", type=Path, default=ROOT / "src/avolyn_headless.cpp")
    parser.add_argument("--profile", type=Path, default=ROOT / "avolyn-release-profile.json")
    parser.add_argument("--boot-app0", required=True, type=Path)
    parser.add_argument("--output-dir", type=Path, default=Path("out"))
    parser.add_argument("--version", default="")
    args = parser.parse_args()
    try:
        report = candidate(args.build, args.source, args.profile, args.boot_app0, args.output_dir, args.version,
                           os.environ.get("GITHUB_SHA", ""), os.environ.get("GITHUB_REPOSITORY", ""), os.environ.get("GITHUB_RUN_ID", ""))
    except (ValueError, OSError) as exc:
        parser.error(str(exc))
    if os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as file:
            file.write(f"version={report['version']}\n")
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as file:
            file.write(f"## Avolyn firmware candidate {report['version']}\n\n"
                       f"Requires desktop **{report['compatibility']['minimumDesktopVersion']} or newer** with supported protocols.\n\n"
                       f"Source commit: `{report['sourceCommit']}`.\n\n"
                       "Download **avolyn-slate-firmware-candidate** from this run's Artifacts section and extract it. "
                       "Use the complete `.avolyn-firmware` file. Checksums, compatibility and source information are included.\n\n"
                       "Physical display refresh is disabled. Publication and hardware installation each require separate approval.\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
