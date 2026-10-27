# Headless USB frame receiver (0.2.6-dev)

Target: XIAO ESP32-S3, Waveshare 5.79-inch 792 × 272 B/W profile. Both `avolyn_headless` and its existing `avolyn_slate_579` alias remain **headless**: neither build drives a physical panel.

Version 0.2.6-dev fixes USB command loss under burst input. The pinned Arduino ESP32 2.0.17 HWCDC driver defaults to a 256-byte RX queue and drops bytes from an interrupt when that queue fills. A successful small FRAME_CAPS request therefore does not establish that a larger BEGIN or chunk will survive. See [HWCDC.cpp](https://github.com/espressif/arduino-esp32/blob/2.0.17/cores/esp32/HWCDC.cpp), `begin()` and `hw_cdc_isr_handler()`.

The firmware now requests a 2,048-byte RX queue before `Serial.begin()` enables its interrupts, preserving the 1,024-byte command limit plus newline. Configuration occurs once at boot; no USB host wait is introduced. FRAME_CAPS and GET_STATUS report `usbRxBufferBytes` and `usbReceiveReady`. If the allocation fails, frame storage is not advertised and BEGIN is rejected. USB parse/line-limit errors have stable codes without echoing input.

Host regression tests model a burst into the pinned driver's bounded queue: small capability queries survive at 256 bytes, larger manifests/chunks lose bytes, and the new configuration preserves complete commands including the maximum-length line. The actual JSON bridge/receiver is exercised downstream of that queue model. Allocation failure, checksum, offset, retry and rollover tests remain enabled. Hardware confirmation is still required.

This milestone adds `avolyn-frame-v1` JSON commands on USB only: FRAME_CAPS, FRAME_BEGIN, FRAME_CHUNK, FRAME_COMMIT, FRAME_CANCEL and FRAME_STATUS. No LAN frame route, automatic firmware installation, reboot, NVS image storage or physical refresh is added.

The matching [desktop PR #14](https://github.com/DominicD213/AvolynSlateDesktopApp/pull/14) documents the wire contract in `docs/usb-layout-upload.md`. Frames use profile `waveshare-579-bw`, format `mono1-msb-white1`, width 792, height 272, stride 99 and exactly 26,928 bytes. SHA-256 is required. Each chunk contains canonical Base64 encoding of at most 512 bytes, keeping commands below the existing 1,024-byte USB line limit.

The single-owner receiver runs only in the Arduino loop. It stages one image, checks every offset and digest, and atomically replaces the previous complete frame only after verification. Active plus staging pixel memory is 53,856 bytes, excluding runtime overhead. Allocation failure returns an error. Staging expires after 15 seconds idle or 120 seconds total using rollover-safe millis arithmetic. No NVS writes occur during transfer.

Retries of identical received chunks and committed transfers are idempotent. A busy BEGIN, conflicting duplicate, gap, incomplete frame or corrupt digest cannot overwrite the previous valid image. Cancelling does not erase the active image. Restarting clears RAM. FRAME_STATUS does not extend transfer lifetime.

Capabilities report `storageSupported: true`, `persistent: false`, `displaySupported: false` and a conservative `minRefreshIntervalMs: 180000`. All replies preserve `displayRefreshedFrameId: ""`. Receipt must never be presented as physical display success.

Run `pio run -e avolyn_headless` to compile without upload. After its dependencies are installed, `python scripts/test_frame_receiver.py` compiles the actual ArduinoJson bridge and portable receiver for the host and exercises bounds, corruption, retries, replacement and timer rollover with sanitizers. It requires g++, libmbedtls-dev and the PlatformIO ArduinoJson dependency.

For user-approved hardware testing, install the reviewed candidate through the desktop's existing firmware installer, connect a USB data cable, choose a 792 × 272 canvas and press **Upload current layout via USB** in Your Slate. Verify checksum-confirmed receipt, unplug during a second upload and confirm the previous frame remains accepted while the board stays powered. Wi-Fi/status recovery should continue after transfer. These hardware checks have not been performed by the agent.

Physical driver support, GPIO wiring verification, encrypted Wi-Fi frame delivery and scheduled refresh are subsequent work. No firmware has been flashed as part of this PR.
