# Avolyn firmware candidates and approval

Firmware changes on a pull request or `main` automatically run **Avolyn firmware release candidate**. Relevant inputs include source, headers, libraries, board configuration, `platformio.ini`, the release profile, packaging scripts and release workflows. This builds the real `avolyn_headless` XIAO target, verifies its factory images and retains a downloadable artifact for 30 days. It does not install firmware or publish a release.

## Download a candidate

1. Open this repository's **Actions** tab, then **Avolyn firmware release candidate**.
2. Choose a successful run for the approved commit. The job summary shows the firmware version, minimum desktop version and source commit. PR runs are development candidates built from GitHub's test merge; after merge, use the new `main` run.
3. Scroll to **Artifacts** and download the artifact named **avolyn-slate-firmware-candidate-<version>-attempt-<number>** from the current run attempt. The summary gives its exact name. Artifacts are separate from **Releases**.
4. Extract the ZIP. Installable firmware is `Avolyn-Slate-<version>.avolyn-firmware`. Keep the adjacent `.release.json`, `SHA256SUMS.txt` and `candidate-info.json` for verification. A raw `firmware.bin`, JSON manifest or ZIP is not the complete installation package.
5. In the updated desktop app, review the package under **Your Slate** before approving installation. Choosing an alternate local package is a Developer control. A production app can include an approved package for offline setup.

The candidate remains headless. Physical screen refresh is disabled. Radio testing requires the XIAO antenna and a suitable 2.4 GHz LAN; physical display testing requires the actual panel and its verified driver.

## Compatibility and provenance

`avolyn-release-profile.json` declares the minimum desktop version, exact protocol IDs, three preset slots and disabled physical refresh. The candidate builder checks that the declared protocols/slot count match the firmware headers. It also rejects an application image that does not contain the source's declared `FIRMWARE_VERSION`, helping catch stale build output. Existing chip, image-address, partition, OTA-data, overlap and SHA-256 checks remain in force.

Both the package and release manifest carry the same compatibility declaration. `candidate-info.json` identifies the checked-out commit, repository, workflow run/attempt, artifact name, environment and package digest. Reruns keep earlier artifacts and use distinct names. Draft-release jobs consume the exact artifact ID returned by the successful build, including when only the publication job is retried. The artifact contains per-package and per-manifest checksums. These are integrity/provenance records, not a code-signing certificate or proof of hardware acceptance.

Desktop `0.7.1-dev.4` adds a compatibility review before installation and blocks declared requirements it cannot support. The current firmware contract requires at least desktop `0.7.1-dev.3`. The desktop repository also provides an offline verifier:

```powershell
npm.cmd run firmware:check-candidate -- "C:\Downloads\candidate\Avolyn-Slate-0.2.8-dev.avolyn-firmware"
```

This uses the actual desktop package parser, verifies the adjacent manifest and checks supported compatibility. After approval, `firmware:bundle` can include that verified package in a production desktop build. Bundling requires declared, supported compatibility. Existing local legacy packages remain importable in the app and are explicitly labeled as having undeclared app compatibility.

## Publication stays manual

The workflow's **Run workflow** option builds a candidate without publishing by default. Leave **version** empty to use the checked-out source version, or enter an exact version assertion. A mismatch fails the build. Checking **Create draft release** explicitly requests a separate job with write permission; it consumes the already built artifact and refuses to overwrite an existing tag/release. The draft is a prerelease. Publishing it and deciding whether it belongs on a stable update channel are separate maintainer actions.

The inherited TRMNL engineering/development/tag publishing jobs no longer run on merges or tag pushes. They require a manual `build` run with an explicit `upstream_publish` selection, and the called workflows check the approval flag again. The direct development-publishing and upstream release-publishing workflows both default their approval checkboxes to false. The latter validates and checks out an exact numeric tag under `refs/tags/` before building or accessing AWS. These routes target upstream TRMNL artifacts/infrastructure and are separate from Avolyn candidates. Use `none` for ordinary builds. AWS credentials are not required for Avolyn candidate builds.
