# Avolyn Slate headless Wi-Fi recovery (0.2.3-dev)

## Independent recovery loops

**XIAO ESP32-S3 firmware** is responsible for Wi-Fi association. It immediately tries saved 2.4 GHz credentials after boot, and retries indefinitely (not three times) if the router is unavailable or the power supply changes. A failed attempt can be detected early after a short grace interval for `WL_CONNECT_FAILED` / `WL_NO_SSID_AVAIL`; all other failed joins time out after 15 seconds. Retry delays grow 2, 4, 8, 16, then 30 seconds maximum. Credentials are cached in RAM while running to avoid NVS reads on each retry; they are written to NVS only after a successful, *changed* manually requested network. Wi-Fi scanning happens only when the desktop asks via USB and is **not** part of normal reconnection.

**Avolyn Desktop** is responsible for authenticated LAN reachability. The separate desktop PR makes up to three relatively frequent secure checks after a failure, then transitions to a maintenance check approximately once per minute while offline. When authenticated again, it switches to a healthy-link check roughly once every 45 seconds. These are desktop **network status checks**, *not* a limit on firmware Wi-Fi association. There is no automatic USB provisioning or rotation of pairing credentials.

## CPU behavior

The firmware yields to FreeRTOS for 10 ms each main-loop iteration, handles USB serial promptly, and samples the Wi-Fi state only once per 250 ms (four times per second), rather than polling 500 times per second. The HTTP server is started on Wi-Fi association and shut down on disconnection; no background network scans or pairing-key rotation are scheduled. This is designed to reduce wasted CPU work, not a verified battery-power benchmark. The Wi-Fi radio remains powered for reliable LAN operation.

## Connection timing diagnostics

A USB `WIFI_STATUS` reply includes `connectionElapsedMs`, `lastJoinDurationMs`, `hasJoinDuration`, `lastFailedAttemptMs`, a numeric last failed Wi-Fi status, and connected RSSI in dBm. This reports actual XIAO association timing separately from the desktop LAN authentication check. No network password or cryptographic pairing key is returned.

## Limits and test plan

The desktop presently uses the previously USB-learned IP address. Router DHCP IP changes, firewall isolation, or LAN reachability problems can still require USB repair or future authenticated LAN discovery. Pairing secrets remain in NVS; no flash security or display functionality is added.

No firmware is flashed or released by this change. After approval, build 0.2.3-dev as a draft and test with a stable 5V power source: normal startup, unplug/replug, router loss beyond three firmware attempts, router restoration, encrypted desktop status, Wi-Fi password changes, and no USB during maintenance. A successful PlatformIO build alone is not a hardware validation.
