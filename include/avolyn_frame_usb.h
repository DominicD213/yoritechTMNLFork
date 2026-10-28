#pragma once
#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>

#include "avolyn_frame_receiver.h"
#include "avolyn_preset_store.h"
#include "avolyn_usb_transport.h"

namespace avolyn {
  constexpr char FRAME_PROTOCOL[] = "avolyn-frame-v1";
  constexpr char FRAME_PROFILE[] = "waveshare-579-bw";
  constexpr char FRAME_FORMAT[] = "mono1-msb-white1";
  constexpr char PRESET_PROTOCOL[] = "avolyn-presets-v1";

  inline bool frameSha256(const uint8_t *data, size_t length, char *hex) {
    uint8_t digest[32];
    const auto *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md || mbedtls_md(md, data, length, digest) != 0) return false;
    constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
      hex[i * 2] = digits[digest[i] >> 4];
      hex[i * 2 + 1] = digits[digest[i] & 15];
    }
    hex[64] = 0;
    return true;
  }

  inline const char *frameText(JsonVariantConst value, size_t length) {
    if (!value.is<const char *>() || value.as<JsonString>().size() != length) return "";
    return value.as<const char *>();
  }

  inline bool frameDecode(JsonVariantConst value, uint8_t *bytes, size_t &length) {
    if (!value.is<const char *>()) return false;
    const JsonString text = value.as<JsonString>();
    if (!text.size() || text.size() > 684 || text.size() % 4) return false;
    if (mbedtls_base64_decode(bytes, FRAME_CHUNK_BYTES, &length, reinterpret_cast<const uint8_t *>(text.c_str()),
                              text.size()) != 0 ||
        !length || length > FRAME_CHUNK_BYTES)
      return false;
    uint8_t canonical[685];
    size_t encoded = 0;
    // Canonical encoding rejects whitespace, embedded NUL, bad padding and unused bits.
    return mbedtls_base64_encode(canonical, sizeof(canonical), &encoded, bytes, length) == 0 &&
           encoded == text.size() && std::memcmp(canonical, text.c_str(), encoded) == 0;
  }

  inline const char *activatePreset(PresetStore &store, FrameReceiver &frames, unsigned n, const char *expectedSha,
                                    bool remember = true) {
    if (frames.busy()) return "busy";
    if (!store.ready()) return "preset_storage_unavailable";
    if (n < 1 || n > 3 || !store.slot(n).occupied || !store.slot(n).valid) return "preset_not_available";
    if (!frameHex(expectedSha, 64) || std::strcmp(store.slot(n).sha256, expectedSha)) return "preset_changed";
    auto bytes = store.load(n);
    if (!bytes) return "preset_read_failed";
    if (remember) {
      const char *error = store.select(n, expectedSha);
      if (error) return error;
    }
    const auto &r = store.slot(n);
    return frames.installActive(std::move(bytes), r.frameId, r.sha256, n);
  }

  inline void presetList(JsonDocument &reply, const PresetStore &store) {
    auto list = reply["slots"].to<JsonArray>();
    for (unsigned n = 1; n <= 3; ++n) {
      const auto &r = store.slot(n);
      auto item = list.add<JsonObject>();
      item["slot"] = n;
      item["occupied"] = r.occupied;
      item["valid"] = r.valid;
      item["presetId"] = r.presetId;
      item["presetName"] = r.name;
      item["frameId"] = r.frameId;
      item["sha256"] = r.sha256;
    }
  }

  inline const char *restoreSavedPreset(PresetStore &store, FrameReceiver &frames) {
    if (!store.ready()) return "preset_storage_unavailable";
    unsigned selected = store.selectedSlot();
    if (!selected || !store.slot(selected).valid) {
      selected = 0;
      for (unsigned n = 1; n <= 3; ++n)
        if (store.slot(n).valid) {
          selected = n;
          break;
        }
    }
    return selected ? activatePreset(store, frames, selected, store.slot(selected).sha256, false) : nullptr;
  }

  inline bool handleFrameUsb(const JsonDocument &command, FrameReceiver &frames, uint32_t now, JsonDocument &reply,
                             size_t usbRxBufferBytes, PresetStore *presets = nullptr) {
    const char *name = command["cmd"] | "";
    if (std::strncmp(name, "FRAME_", 6) != 0 && std::strncmp(name, "PRESET_", 7) != 0) return false;
    frames.expire(now);
    const char *requestId = frameText(command["requestId"], 32);
    const char *id = frameText(command["transferId"], 32);
    const char *error = nullptr;
    if (command["cmd"].as<JsonString>().size() != std::strlen(name)) {
      error = "invalid_command";
    } else if (!frameHex(requestId, 32)) {
      error = "invalid_request";
    } else if (std::strcmp(name, "FRAME_CAPS") == 0) {
      reply["protocol"] = FRAME_PROTOCOL;
      reply["profile"] = FRAME_PROFILE;
      reply["width"] = FRAME_WIDTH;
      reply["height"] = FRAME_HEIGHT;
      reply["stride"] = FRAME_STRIDE;
      reply["pixelFormat"] = FRAME_FORMAT;
      reply["maxFrameBytes"] = FRAME_BYTES;
      reply["maxChunkBytes"] = FRAME_CHUNK_BYTES;
      reply["storageSupported"] = usbFrameReceiveReady(usbRxBufferBytes);
      reply["usbRxBufferBytes"] = usbRxBufferBytes;
      reply["usbReceiveReady"] = usbFrameReceiveReady(usbRxBufferBytes);
      reply["persistent"] = false;
      reply["presetProtocol"] = PRESET_PROTOCOL;
      reply["maxPresets"] = PRESET_SLOTS;
      reply["presetStorageReady"] = presets && presets->ready();
      reply["minRefreshIntervalMs"] = 180000;
    } else if (std::strcmp(name, "FRAME_BEGIN") == 0 && !usbFrameReceiveReady(usbRxBufferBytes)) {
      error = "usb_transport_not_ready";
    } else if (std::strcmp(name, "FRAME_BEGIN") == 0) {
      PresetTarget target;
      if (!command["slot"].isNull()) {
        const JsonString title = command["presetName"].as<JsonString>();
        if (!command["slot"].is<unsigned>() || command["slot"].as<unsigned>() < 1 ||
            command["slot"].as<unsigned>() > 3 ||
            std::strcmp(frameText(command["presetProtocol"], sizeof(PRESET_PROTOCOL) - 1), PRESET_PROTOCOL) ||
            !frameHex(frameText(command["presetId"], 32), 32) || !title.c_str() || title.size() < 1 ||
            title.size() > 64 || title.size() != std::strlen(title.c_str()) || !presetNameValid(title.c_str()))
          error = "invalid_preset";
        else if (!presets || !presets->ready())
          error = "preset_storage_unavailable";
        else {
          target.slot = command["slot"].as<unsigned>();
          std::memcpy(target.presetId, frameText(command["presetId"], 32), 33);
          std::memcpy(target.name, title.c_str(), title.size() + 1);
        }
      }
      if (!error) {
        if (!command["width"].is<unsigned>() || command["width"].as<unsigned>() != FRAME_WIDTH ||
            !command["height"].is<unsigned>() || command["height"].as<unsigned>() != FRAME_HEIGHT ||
            !command["stride"].is<unsigned>() || command["stride"].as<unsigned>() != FRAME_STRIDE ||
            !command["bytes"].is<unsigned>() || command["bytes"].as<unsigned>() != FRAME_BYTES ||
            std::strcmp(frameText(command["profile"], sizeof(FRAME_PROFILE) - 1), FRAME_PROFILE) ||
            std::strcmp(frameText(command["pixelFormat"], sizeof(FRAME_FORMAT) - 1), FRAME_FORMAT))
          error = "invalid_manifest";
        else
          error = frames.begin(id, frameText(command["sha256"], 64), now, target);
      }
    } else if (std::strcmp(name, "FRAME_CHUNK") == 0) {
      uint8_t bytes[FRAME_CHUNK_BYTES];
      size_t length = 0;
      if (!command["offset"].is<unsigned>() || !frameDecode(command["data"], bytes, length))
        error = "invalid_chunk";
      else
        error = frames.chunk(id, command["offset"].as<unsigned>(), bytes, length, now);
    } else if (std::strcmp(name, "FRAME_COMMIT") == 0) {
      error = frames.commit(id, now);
    } else if (std::strcmp(name, "FRAME_CANCEL") == 0) {
      error = frames.cancel(id);
    } else if (std::strcmp(name, "PRESET_LIST") == 0) {
      if (!presets || !presets->ready())
        error = "preset_storage_unavailable";
      else
        presetList(reply, *presets);
    } else if (std::strcmp(name, "PRESET_SELECT") == 0) {
      if (!command["slot"].is<unsigned>())
        error = "invalid_preset";
      else if (!presets)
        error = "preset_storage_unavailable";
      else
        error = activatePreset(*presets, frames, command["slot"].as<unsigned>(), frameText(command["sha256"], 64));
      if (!error) presetList(reply, *presets);
    } else if (std::strcmp(name, "FRAME_STATUS") != 0) {
      error = "unsupported_command";
    }
    reply["type"] = "FRAME_REPLY";
    reply["cmd"] = command["cmd"];
    reply["requestId"] = requestId;
    reply["ok"] = error == nullptr;
    if (error) reply["error"] = error;
    reply["nextOffset"] = frames.nextOffset();
    reply["receivingTransferId"] = frames.receivingId();
    reply["acceptedFrameId"] = frames.acceptedId();
    reply["sha256"] = frames.acceptedDigest();
    if (std::strncmp(name, "PRESET_", 7) == 0) {
      reply["presetProtocol"] = PRESET_PROTOCOL;
      reply["maxPresets"] = PRESET_SLOTS;
      reply["presetStorageReady"] = presets && presets->ready();
    }
    reply["activePresetSlot"] = frames.activeSlot();
    reply["selectedPresetSlot"] = presets ? presets->selectedSlot() : 0;
    if (!error && (std::strcmp(name, "FRAME_COMMIT") == 0 || std::strcmp(name, "PRESET_SELECT") == 0)) {
      reply["persistent"] = frames.activeSlot() > 0;
      if (frames.activeSlot() && presets) reply["storedFrameId"] = presets->slot(frames.activeSlot()).frameId;
    }
    reply["displaySupported"] = false;
    reply["displayRefreshedFrameId"] = "";
    return true;
  }
} // namespace avolyn
