# XIAO wall-power recovery — 0.2.4-dev

The headless firmware loads saved Wi-Fi and USB-provisioned pairing credentials at boot and reconnects without a computer. Disconnect callbacks only update bounded atomic state; the main loop owns recovery, HTTP restart and mDNS startup. Failed Wi-Fi connections continue retrying indefinitely with a capped 30-second backoff. There is no three-attempt ceiling on the board.

## Changes

- Announce `avolyn-slate-<12 lowercase MAC hex digits>.local` and `_avolyn-slate._tcp` on port 8080 after Wi-Fi and the authenticated HTTP routes are ready. Tear down discovery/HTTP on a link loss and restart them after recovery. Discovery contains no pairing credentials; the desktop must authenticate an encrypted response before trusting an address change.
- Persist SSID/password together in one checked NVS record, with read-back verification. Load existing `ssid`/`pass` settings and migrate them at boot. Failed storage operations are reported, rather than presenting a Wi-Fi join as proof that settings survived a reboot. Forget removes both record formats before reporting success.
- Avoid a separate SDK Wi-Fi flash configuration path. A firmware-controlled scheduler owns saved-network recovery. A join can spend up to 30 seconds associating and, once associated, up to 15 seconds obtaining an address, with a total 45-second ceiling. This prevents the previous fixed 15-second cutoff from interrupting a slow join; it does not promise a particular RF connection time.
- USB processing/output is conditional on a connected host, with bounded transmit waits. There is no wait-for-serial startup gate. The one-time boot banner follows the initial saved-network attempt. Normal status replies remain available on explicit requests; successful joins and link loss produce one transition notification.
- Authenticated status includes bounded reset reason, uptime, Wi-Fi disconnect reason and saved-settings status, allowing investigation after wall-power recovery. No passwords or raw pairing data are included.

## Evidence and limits

The supplied 0.2.3-dev log shows two approximately 15-second failed attempts, then a successful join at 10.0.0.21, with signal around -77/-78 dBm. It does not show the board's state on the external supply. The previous desktop also only knew the address saved during USB pairing, so a changed DHCP lease could appear as a failed board connection. Both sides require the corresponding recovery changes for automatic address rediscovery.

After approval, test USB setup/pairing, a cold boot on stable 5 V USB wall power, router loss/return, a changed DHCP lease, and desktop sleep/resume. Verify that reconnection reuses the existing pairing key. Repeated `AVOLYN_SLATE_HEADLESS_READY` banners mean repeated boots; inspect reset reason and power rather than suppressing those markers. Software checks cannot establish external supply stability, antenna performance, router isolation or physical display compatibility.

## Validation

`pio run -e avolyn_headless -j 4` compiles only; it never uploads. Run `python scripts/test_wifi_reconnect.py`, `python scripts/test_wifi_credentials.py` (after PlatformIO resolves ArduinoJson), `python scripts/test_pairing_crypto.py` with host Mbed TLS installed, and the release packaging unit tests. GitHub pairing checks compile the actual XIAO target and run these firmware behavior checks. No merge, firmware publication or flashing is authorized by this change.
