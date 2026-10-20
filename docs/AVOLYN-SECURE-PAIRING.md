# Secure wireless pairing, development protocol v1

This PR connects the XIAO ESP32-S3 headless firmware to the Electron application with authenticated, encrypted status requests. USB is the initial trust channel. Pairing does not enable image delivery, change display wiring, publish firmware, or automatically install anything.

## User flow

1. After an explicitly approved firmware installation, connect USB and let Avolyn identify the board. Join a 2.4 GHz Wi-Fi network from the app if needed.
2. Choose **Pair this Slate securely**. The main process reads the current board identity and LAN address, saves an encrypted recovery record, provisions a new random key over USB, checks its acknowledgement, and verifies an encrypted LAN response.
3. Keep Slate powered from the wall, unplug computer USB, and choose **Test wireless connection**. Saved pairing works across desktop restarts. The UI reports the time of the most recent authenticated response, not continuous reachability.
4. Pair again over USB if DHCP changes the board's address or to replace the key. No network scan, manually entered MAC address, or unauthenticated address change is accepted.
5. **Remove pairing over USB** revokes the board's key before deleting the matching desktop record. Wi-Fi does not have to be connected for removal. Forgetting Wi-Fi is a separate action.

A board has one pairing slot. Re-pairing replaces the previous desktop key. The app remembers one active pairing plus a pending replacement for interruption recovery. If an acknowledgement is lost, retry over USB or test the pending key over LAN. If LAN verification fails after the acknowledgement, the saved pairing remains available to retry and is not presented as a successful network check.

## Trust and storage

- Electron generates a 32-byte random secret and 16-byte random key ID. Neither is supplied by the renderer.
- Pairing records are atomically replaced after encryption with Electron `safeStorage`. Pairing refuses an unavailable credential store or Linux's `basic_text` fallback. No plaintext fallback exists.
- Renderer IPC only exposes status, pair, verify, and revoke operations. Secrets are excluded from status, preset export, USB acknowledgements, and diagnostics. Existing sandbox, context isolation, sender validation, and firmware-install approval remain in place.
- Firmware writes key ID plus secret as one NVS value before acknowledging USB `PAIR_SET`. `PAIR_FORGET` erases that value and invalidates the outstanding challenge. Provisioning and revocation are USB-only operations.
- This development target does not enable flash encryption or secure boot. NVS secrets can be extracted by an attacker with sufficient physical access. USB access is trusted and permits pairing replacement. Production physical-device protection requires a separately validated configuration.

## Wire contract

The transport is HTTP on port 8080 with AES-256-GCM encrypted application messages. It is not TLS. LAN status cannot fall back to the former plaintext MAC-only check. Only private numeric IPv4 destinations learned over USB are accepted by Electron.

`GET /challenge` returns `{"protocol":"avolyn-pair-v1","challenge":"<32 random bytes as lowercase hex>"}` after pairing. It exposes no device identity or key. The board retains a challenge for at most 10 seconds, consumes it once after authenticating a valid status request, and discards it on reboot or pairing change. Repeated challenge reads do not invalidate a legitimate in-progress request.

`POST /status` and its response use the envelope:

```json
{"v":1,"keyId":"<16 bytes hex>","iv":"<12 bytes hex>","ciphertext":"<hex>","tag":"<16 bytes hex>"}
```

Request plaintext is `{"op":"status","challenge":"<challenge>"}`. Response plaintext contains product, board, firmware version, MAC, Wi-Fi connection, and paired status. Electron checks the authenticated identity against its USB-observed identity.

Directional AES keys use HKDF-SHA256 with the 32-byte master secret, salt equal to the UTF-8 key-ID hex string, and info equal to `avolyn-pair-v1/request` or `avolyn-pair-v1/response`. Each message uses a fresh 12-byte random IV and full 16-byte authentication tag. Keys differ between directions.

Associated data is UTF-8 with LF separators and no trailing LF:

- Request: `avolyn-pair-v1\nrequest\n<keyId>\n<challenge>`
- Response: `avolyn-pair-v1\nresponse\n<keyId>\n<challenge>\n<requestIv>`

The response is bound to the exact request IV and challenge. Plaintext is parsed only after authentication succeeds. Captured requests fail after challenge consumption, expiry, reboot, or key replacement. Plaintext legacy responses, modified ciphertext/tags, reflected messages, and responses from a different transaction are rejected.

Electron bounds streamed response bytes and uses one 5-second deadline across challenge and status requests, with redirects rejected. The firmware uses ESP-IDF's HTTP parser, checks content length before reading into a fixed buffer, limits plaintext to 1024 bytes and envelopes to 3072 bytes, bounds socket waits, and rate-limits authentication attempts. A mutex serializes LAN access with USB key changes. No LAN pairing, reset, configuration, firmware-install, or image-update endpoint is exposed.

## Validation

Desktop: `npm ci`, `npm test`, `npm run build`.

Firmware: `pio run -e avolyn_headless` (compile only). `avolyn_slate_579` is retained as an alias for this headless development target. The actual XIAO board definition is used, and stock TRMNL display libraries are excluded. No display compatibility claim follows from this build.

Firmware cryptographic interoperability: install Mbed TLS 2.28+ development headers and run `python scripts/test_pairing_crypto.py`. The test compiles the actual firmware crypto header against Mbed TLS and checks the same public deterministic vector used by the desktop tests. It covers both directions, HKDF, tag/ciphertext tampering, reflection, challenge reuse, expiry, and timer wrap. The fixture's dummy keys are public test data and must never be provisioned on a board.

CI checks the desktop tests/build, firmware formatting, the stock TRMNL build/native tests, and the new headless target plus host crypto tests. Physical USB, Wi-Fi, wall-power-only operation, reboot recovery, Windows credential-store persistence, key replacement, and revocation still require the user's hardware validation after approval. Nothing in these checks flashes a device.
