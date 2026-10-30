# Secure Wi-Fi presets

Screen builder's **Presets on your Slate** panel uploads, lists, activates and deletes the three persistent snapshots over USB or authenticated Wi-Fi. An open USB connection takes priority. With USB unplugged, the paired Slate can remain on wall power. The existing pairing is authenticated again, including discovery/DHCP address recovery, before an operation. No plaintext image-service fallback is used.

Each displayed catalog has an opaque destination token. Upload, replace, select and delete requests must retain that token; a USB reconnect, different board, pairing replacement or switch between USB and Wi-Fi requires a fresh catalog and confirmation. A failed USB upload is not automatically replayed over Wi-Fi. Confirmations retain the slot's frame ID/checksum, and Wi-Fi firmware checks the expected slot inside the same device lock as BEGIN. New firmware also honors this expectation over USB.

## Wire contract: avolyn-presets-lan-v1

The paired board exposes `GET /preset-challenge` and `POST /presets` on port 8080. These are independent of the older status protocol, whose limits and keys remain unchanged. The challenge response contains only the protocol and a 32-byte lowercase-hex random challenge. It lives in RAM for 10 seconds and is consumed once after successful authentication, including authenticated malformed commands. Repeated reads do not invalidate a pending challenge. Boot, Wi-Fi disconnection and pairing changes invalidate it.

The encrypted envelope is `{v:1,keyId,contextId,iv,ciphertext,tag}`. Key ID and context ID are 16-byte lowercase hex; IV is 12 bytes and tag is 16 bytes. Ciphertext is lowercase hex of at most 2,048 plaintext bytes. The entire HTTP request/response is bounded to 4,608 bytes. Content type is exactly `application/json`; nesting, reads, socket waits and failed authentication attempts are bounded. HTTP is the transport, with AES-256-GCM protecting the application messages; this is not TLS.

HKDF-SHA256 uses the USB-provisioned 32-byte master secret, UTF-8 key-ID hex as salt, and UTF-8 `avolyn-presets-lan-v1/<contextId>/<direction>` as info. Direction is `request` or `response`. Each upload uses its transfer ID as context; management requests use a new random context. Thus keys differ by protocol purpose, transfer and direction. Every send, including retries, uses a fresh random IV and a newly obtained challenge. The master secret never crosses the LAN or renderer bridge.

AAD is UTF-8 with LF separators and no trailing LF:

```
avolyn-presets-lan-v1
<direction>
<keyId>
<challenge>
<contextId>
```

Responses append LF plus the exact request IV. Request plaintext contains the existing frame/preset command, 16-byte hex request ID, and challenge. Transfer mutations must have `transferId === contextId`. Replies authenticate protocol, board MAC, command and request ID. The desktop verifies all four before accepting an acknowledgement. Public deterministic vectors are identical in both repositories; their dummy credentials must never be provisioned on hardware.

Allowed commands are `FRAME_CAPS`, `FRAME_BEGIN`, `FRAME_CHUNK`, `FRAME_COMMIT`, `FRAME_CANCEL`, `FRAME_STATUS`, `PRESET_LIST`, `PRESET_SELECT` and `PRESET_DELETE`. Wi-Fi BEGIN requires a persistent slot (1–3), preset metadata and expected occupancy/frame ID/SHA-256. The 792 × 272 image remains exactly 26,928 packed bytes in at most 512-byte chunks. Commands reuse the USB receiver and atomic, verified preset catalog. LAN has no pairing, Wi-Fi configuration, reset or firmware-install route.

## Interruption and resource policy

One application preset operation owns the transport. Background LAN probes pause during it; firmware installation is blocked until it finishes. Firmware uses one mutex for pairing keys, USB/LAN frame commands, catalog access and receiver expiry. Network and USB cannot write storage concurrently. Receiver time is sampled while holding that mutex.

The desktop gives an operation at most 90 seconds. Each command retries at most twice after network failure or transient HTTP 429/503, with fresh encryption. Status and ordinary commands have a 5-second network deadline; durable commit/select/delete allow 15 seconds. Identical chunks and repeated commits are idempotent. If all commit acknowledgements are lost, authenticated FRAME_STATUS must confirm the same transfer, checksum and durable slot before success is reported. Lost delete acknowledgement can be reconciled only against an authenticated catalog showing the target empty. A changed occupied slot is never deleted as a retry.

Progress advances only on acknowledged offsets. Receiver staging expires after 15 seconds idle or 120 seconds total. Short interruptions can recover within command retries; expired staging or application restart requires a new upload. Uncommitted bytes never replace a saved image. Persistent commit uses the existing read-back/checksum/fsync/atomic-rename path. Success means verified device storage, not physical screen refresh.

## Validation and hardware acceptance

Desktop tests cover vectors, bounds, tampering, identity/request binding, fresh-nonce retries, lost commit/delete acknowledgements, destination changes, exclusive access and firmware-operation guards. Firmware's `scripts/test_preset_lan.py` compiles the actual crypto/command/storage implementation with address/undefined-behavior sanitizers and checks it against Node messages. Optional `AVOLYN_DESKTOP_ROOT` runs the actual desktop client and transfer engine against that C++ handler. The original storage, USB, pairing and Wi-Fi recovery tests remain enabled.

The firmware target remains headless. After separately approving firmware installation, hardware acceptance requires the XIAO antenna and a stable 2.4 GHz LAN: pair over USB, disconnect computer USB, save three named presets, activate/delete over Wi-Fi, restart on wall power and verify the catalog/selection. Interrupt an upload and confirm earlier snapshots survive. Physical Waveshare refresh needs the actual panel and a separately verified driver; it is not enabled here. No test or packaging command flashes a board or publishes a release.
