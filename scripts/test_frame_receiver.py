#!/usr/bin/env python3
"""Test the actual JSON bridge and receiver on the host; never accesses hardware."""
import base64
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
fixture = json.loads((root / "scripts/fixtures/frame-v1.json").read_text())
source = r'''
#include "avolyn_frame_usb.h"
#include <cassert>
#include <iostream>
#include <string>
// Model the pinned HWCDC ISR: enqueue a burst, drop bytes after RX queue fills,
// and let the application drain it later (e.g. after Wi-Fi work in loop()).
struct BufferedUsb {
  size_t capacity = 0;
  bool started = false, failAllocation = false;
  uint32_t txTimeout = 0;
  std::string queue;
  size_t setRxBufferSize(size_t bytes) {
    assert(!started); // Production must configure before RX interrupts start.
    if (failAllocation) return 0;
    capacity = bytes;
    return bytes;
  }
  void begin(unsigned long baud) {
    assert(baud == 115200);
    started = true;
    if (!capacity) capacity = 256; // Arduino ESP32 2.0.17 HWCDC default.
  }
  void setTxTimeoutMs(uint32_t ms) { txTimeout = ms; }
  std::string burst(const std::string &bytes) {
    assert(started);
    for (char c : bytes) if (queue.size() < capacity) queue.push_back(c);
    std::string received;
    received.swap(queue);
    return received;
  }
};
int main() {
  avolyn::FrameReceiver frames(avolyn::frameSha256);
  BufferedUsb serial;
  const size_t rxBytes = avolyn::beginUsbSerial(serial);
  assert(serial.txTimeout == 10 && avolyn::usbFrameReceiveReady(rxBytes));
  BufferedUsb legacy;
  legacy.begin(115200);
  const std::string maxLine(avolyn::USB_SERIAL_LINE_MAX, 'x');
  assert(serial.burst(maxLine + "\n") == maxLine + "\n");
  assert(legacy.burst(maxLine + "\n").size() == 256);
  BufferedUsb failed;
  failed.failAllocation = true;
  const size_t failedBytes = avolyn::beginUsbSerial(failed);
  assert(!avolyn::usbFrameReceiveReady(failedBytes));
  JsonDocument caps, failedReply;
  caps["cmd"] = "FRAME_CAPS";
  caps["requestId"] = "00000000000000000000000000000001";
  assert(avolyn::handleFrameUsb(caps, frames, 0, failedReply, failedBytes));
  assert(failedReply["storageSupported"] == false);
  assert(failedReply["usbReceiveReady"] == false);
  bool reproducedLegacyDrop = false;
  std::string line;
  while (std::getline(std::cin, line)) {
    JsonDocument command, reply;
    const std::string wire = line + "\n";
    const std::string received = serial.burst(wire);
    assert(received == wire); // Entire BEGIN and 512-byte chunks survive bursts.
    assert(!deserializeJson(command, received));
    const std::string oldReceived = legacy.burst(wire);
    if (wire.size() > 256) {
      assert(oldReceived != wire);
      if (std::strcmp(command["cmd"] | "", "FRAME_CAPS") != 0) {
        // A subsequent short polling command supplies a newline after a dropped
        // layout suffix, producing the user's "Unrecognized USB command" path.
        JsonDocument broken;
        assert(deserializeJson(broken, oldReceived + "WIFI_STATUS\n"));
      }
      reproducedLegacyDrop = true;
    } else {
      assert(oldReceived == wire); // Small CAPS queries still worked before fix.
    }
    const uint32_t now = command["_now"].as<uint32_t>();
    if (std::strcmp(command["cmd"] | "", "FRAME_BEGIN") == 0) {
      JsonDocument unavailable;
      avolyn::FrameReceiver noStorage(avolyn::frameSha256);
      assert(avolyn::handleFrameUsb(command, noStorage, now, unavailable, failedBytes));
      assert(unavailable["error"] == "usb_transport_not_ready");
      assert(noStorage.nextOffset() == 0 && noStorage.activePixels() == nullptr);
    }
    assert(avolyn::handleFrameUsb(command, frames, now, reply, rxBytes));
    if (frames.activePixels()) {
      char actual[65] = {};
      assert(avolyn::frameSha256(frames.activePixels(), avolyn::FRAME_BYTES, actual));
      assert(std::strcmp(actual, frames.acceptedDigest()) == 0);
    }
    serializeJson(reply, std::cout);
    std::cout << std::endl;
  }
  assert(reproducedLegacyDrop);
}
'''

with tempfile.TemporaryDirectory(prefix="avolyn-frame-") as folder:
    cpp = Path(folder) / "frame.cpp"
    exe = Path(folder) / "frame"
    cpp.write_text(source)
    subprocess.run([
        os.environ.get("CXX", "g++"), "-std=c++11", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        *shlex.split(os.environ.get("CXXFLAGS", "")),
        "-I" + str(root / "include"),
        "-I" + str(root / ".pio/libdeps/avolyn_headless/ArduinoJson/src"),
        str(cpp), "-lmbedcrypto", "-o", str(exe)
    ], check=True)
    process = subprocess.Popen([str(exe)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    clock = 100
    counter = 0

    def send(cmd, **fields):
        global clock, counter
        counter += 1
        clock += 1
        request_id = f"{counter:032x}"
        command = {"cmd": cmd, "requestId": request_id, **fields, "_now": clock & 0xffffffff}
        assert len(json.dumps(command, separators=(",", ":")).encode()) <= 1024
        process.stdin.write(json.dumps(command, separators=(",", ":")) + "\n")
        process.stdin.flush()
        raw = process.stdout.readline()
        assert raw, f"Receiver stopped on {cmd}"
        reply = json.loads(raw)
        assert reply["type"] == "FRAME_REPLY" and reply["cmd"] == cmd and reply["requestId"] == request_id
        assert reply["displaySupported"] is False and reply["displayRefreshedFrameId"] == ""
        return reply

    def manifest(transfer, image, **changes):
        return {
            "transferId": transfer, "profile": fixture["profile"], "pixelFormat": fixture["pixelFormat"],
            "width": fixture["width"], "height": fixture["height"], "stride": fixture["stride"],
            "bytes": fixture["bytes"], "sha256": hashlib.sha256(image).hexdigest(), **changes
        }

    def chunk(transfer, offset, data):
        return send("FRAME_CHUNK", transferId=transfer, offset=offset, data=base64.b64encode(data).decode())

    def upload(transfer, image):
        assert send("FRAME_BEGIN", **manifest(transfer, image))["nextOffset"] == 0
        for offset in range(0, len(image), 512):
            part = image[offset:offset + 512]
            reply = chunk(transfer, offset, part)
            assert reply["ok"] and reply["nextOffset"] == offset + len(part)

    try:
        caps = send("FRAME_CAPS")
        assert caps["protocol"] == fixture["protocol"] and caps["profile"] == fixture["profile"]
        assert caps["width"] == 792 and caps["height"] == 272 and caps["stride"] == 99
        assert caps["maxFrameBytes"] == 26928 and caps["maxChunkBytes"] == 512
        assert caps["storageSupported"] is True and caps["persistent"] is False
        assert caps["usbReceiveReady"] is True and caps["usbRxBufferBytes"] == 2048
        assert caps["minRefreshIntervalMs"] == 180000
        for example in fixture["base64Examples"]:
            assert base64.b64encode(bytes.fromhex(example["hex"])).decode() == example["base64"]
        image = bytearray([255] * fixture["bytes"])
        for x in fixture["blackPixelsOnFirstRow"]:
            image[x >> 3] &= ~(128 >> (x & 7))
        image[-fixture["stride"]:] = bytes(fixture["stride"])
        first, second, third = "1" * 32, "2" * 32, "3" * 32

        for changes in ({"width": 1360}, {"height": 480}, {"stride": 100}, {"bytes": 26929},
                        {"width": "792"}, {"width": 792.5}, {"width": -1},
                        {"profile": fixture["profile"] + "\u0000extra"}, {"sha256": "z" * 64}):
            assert not send("FRAME_BEGIN", **manifest(first, image, **changes))["ok"]
        assert send("FRAME_BEGIN", **manifest(first, image))["ok"]
        assert send("FRAME_BEGIN", **manifest(first, image))["nextOffset"] == 0
        assert not send("FRAME_BEGIN", **manifest(second, image))["ok"]
        for offset in (-1, 1, 0xffffffff, "0", 1.5):
            assert not chunk(first, offset, image[:1])["ok"]
        for data in ("", "!!!!", "/x==", "/w==\n", "/w==\u0000", base64.b64encode(bytes(513)).decode()):
            assert not send("FRAME_CHUNK", transferId=first, offset=0, data=data)["ok"]
        assert not chunk(second, 0, image[:512])["ok"]
        assert chunk(first, 0, image[:512])["nextOffset"] == 512
        assert chunk(first, 0, image[:512])["nextOffset"] == 512
        assert not chunk(first, 0, bytes(512))["ok"]
        assert send("FRAME_CANCEL\u0000suffix", transferId=first)["error"] == "invalid_command"
        assert send("FRAME_STATUS")["receivingTransferId"] == first
        assert not send("FRAME_COMMIT", transferId=first)["ok"]
        assert not send("FRAME_CANCEL", transferId=second)["ok"]
        for offset in range(512, len(image), 512):
            part = image[offset:offset + 512]
            assert chunk(first, offset, part)["nextOffset"] == offset + len(part)
        accepted = send("FRAME_COMMIT", transferId=first)
        assert accepted["ok"] and accepted["acceptedFrameId"] == first
        assert accepted["sha256"] == hashlib.sha256(image).hexdigest()
        assert send("FRAME_COMMIT", transferId=first)["ok"], "commit retry is idempotent"
        assert send("FRAME_CANCEL", transferId=first)["acceptedFrameId"] == first
        assert not send("FRAME_BEGIN", **manifest(first, image))["ok"], "cannot reuse currently accepted ID"

        # A complete corrupt transfer cannot replace the last valid frame.
        assert send("FRAME_BEGIN", **manifest(second, bytes(len(image))))["ok"]
        for offset in range(0, len(image), 512):
            assert chunk(second, offset, image[offset:offset + 512])["ok"]
        failed = send("FRAME_COMMIT", transferId=second)
        assert failed["error"] == "checksum_mismatch" and failed["acceptedFrameId"] == first

        # Duplicate BEGIN does not extend idle expiry.
        assert send("FRAME_BEGIN", **manifest(third, image))["ok"]
        clock += 14000
        assert send("FRAME_BEGIN", **manifest(third, image))["ok"]
        clock += 1000
        assert send("FRAME_STATUS")["receivingTransferId"] == ""
        assert send("FRAME_STATUS")["acceptedFrameId"] == first

        # Progress cannot extend the total deadline indefinitely.
        assert send("FRAME_BEGIN", **manifest(third, image))["ok"]
        for offset in range(8):
            clock += 14000
            assert chunk(third, offset, image[offset:offset + 1])["ok"]
        clock += 14000
        assert not chunk(third, 8, image[8:9])["ok"]
        assert send("FRAME_STATUS")["receivingTransferId"] == ""

        # Unsigned deadline arithmetic survives millis() rollover.
        clock = 0xfffffff0
        assert send("FRAME_BEGIN", **manifest(third, image))["ok"]
        clock += 100
        assert chunk(third, 0, image[:512])["ok"]
        clock += 15000
        assert send("FRAME_STATUS")["receivingTransferId"] == ""
        assert send("FRAME_STATUS")["acceptedFrameId"] == first
        upload(third, image)
        assert send("FRAME_COMMIT", transferId=third)["acceptedFrameId"] == third
        assert send("FRAME_NOT_SUPPORTED")["error"] == "unsupported_command"
        print("USB burst/drop regression, RX allocation failure, frame JSON, SHA-256, retry and rollover checks passed.")
    finally:
        process.stdin.close()
        return_code = process.wait(timeout=10)
        assert return_code == 0, f"Host receiver exited {return_code}"
