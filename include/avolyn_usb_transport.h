#pragma once
#include <cstddef>

namespace avolyn {
  constexpr size_t USB_SERIAL_LINE_MAX = 1024;
  constexpr size_t USB_RX_BUFFER_BYTES = 2048;
  static_assert(USB_RX_BUFFER_BYTES >= USB_SERIAL_LINE_MAX + 1, "USB RX must hold a complete command and newline");

  // ESP32 Arduino 2.0.17 HWCDC defaults to a 256-byte RX queue. Its ISR drops
  // bytes when full, even if our separate JSON line buffer is much larger.
  // Configure once before begin() enables RX interrupts; never resize a live queue.
  template <typename SerialPort> size_t beginUsbSerial(SerialPort &serial) {
    const size_t allocated = serial.setRxBufferSize(USB_RX_BUFFER_BYTES);
    serial.begin(115200);
    serial.setTxTimeoutMs(10); // Wall power must not wait for a USB host.
    return allocated;
  }

  inline bool usbFrameReceiveReady(size_t bytes) { return bytes >= USB_SERIAL_LINE_MAX + 1; }
} // namespace avolyn
