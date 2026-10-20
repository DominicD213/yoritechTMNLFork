# Avolyn Slate — GitHub firmware release candidates

This Avolyn-only workflow is separate from upstream TRMNL S3 publishing and does not require S3 or TRMNL admin credentials.

## Safety model

- Ordinary commits and pull requests run **package unit tests only**.
- After changes are reviewed and merged, a maintainer can manually run **Actions → Avolyn firmware release candidate → Run workflow** with an exact version matching `src/avolyn_headless.cpp`.
- The workflow compiles `avolyn_headless`, packages actual ESP32-S3 bootloader, partition table, app and matching `boot_app0.bin` when OTA is present, and uploads a short-lived Actions artifact.
- Optional release creation is **draft only**, **prerelease**, and requires an explicit manual choice. No code push or workflow automatically publishes stable firmware or flashes boards.
- Build-generated packages should not be called hardware-verified before being tested on a spare XIAO ESP32-S3.

## Exact asset names

Two matching release assets are required for the desktop updater:
- `Avolyn-Slate-VERSION.avolyn-firmware`
- `Avolyn-Slate-VERSION.avolyn-firmware.release.json`

The release manifest contains `product: avolyn-slate`, `board: xiao-esp32s3`, `chip: ESP32-S3`, source-matched `version`, headless display profile, package filename, SHA-256 hash and byte count.

The desktop updater in `DominicD213/AvolynSlateDesktopApp` looks up the **latest published stable release** via:
`https://api.github.com/repos/DominicD213/yoritechTMNLFork/releases/latest`

Draft and prerelease GitHub Releases are intentionally invisible to this endpoint. Do not publish experimental `-dev` packages as stable customer releases.

## Release checklist before making a draft public

1. Confirm the desktop updater pull request and wireless pairing pull request are tested together.
2. Check source version, panel profile, partition offsets and signed-off package hashes.
3. Manually install the candidate on a spare XIAO and confirm `GET_STATUS` reports the version.
4. Verify reconnect to 2.4 GHz Wi-Fi, secure USB pairing, authenticated status and separate-power operation.
5. Verify recovery when Wi-Fi is unavailable and re-pairing after factory flashing, which may erase device settings.
6. Test downloading the two assets using an unprivileged public browser or updater.
7. Only after a full hardware review and release approval, publish the Release (not a draft/prerelease) and document the tested board/display variants.

Never store Wi-Fi passwords, signing keys, device-pairing secrets or firmware credentials in this repository or release artifacts. Future production firmware should enable and validate secure boot and flash encryption separately.
