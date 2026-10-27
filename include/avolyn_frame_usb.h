#pragma once
#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>

#include "avolyn_frame_receiver.h"

namespace avolyn {
  constexpr char FRAME_PROTOCOL[] = "avolyn-frame-v1";
  constexpr char FRAME_PROFILE[] = "waveshare-579-bw";
  constexpr char FRAME_FORMAT[] = "mono1-msb-white1";

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

  inline bool handleFrameUsb(const JsonDocument &command, FrameReceiver &frames, uint32_t now, JsonDocument &reply) {
    const char *name = command["cmd"] | "";
    if (std::strncmp(name, "FRAME_", 6) != 0) return false;
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
      reply["storageSupported"] = true;
      reply["persistent"] = false;
      reply["minRefreshIntervalMs"] = 180000;
    } else if (std::strcmp(name, "FRAME_BEGIN") == 0) {
      if (!command["width"].is<unsigned>() || command["width"].as<unsigned>() != FRAME_WIDTH ||
          !command["height"].is<unsigned>() || command["height"].as<unsigned>() != FRAME_HEIGHT ||
          !command["stride"].is<unsigned>() || command["stride"].as<unsigned>() != FRAME_STRIDE ||
          !command["bytes"].is<unsigned>() || command["bytes"].as<unsigned>() != FRAME_BYTES ||
          std::strcmp(frameText(command["profile"], sizeof(FRAME_PROFILE) - 1), FRAME_PROFILE) ||
          std::strcmp(frameText(command["pixelFormat"], sizeof(FRAME_FORMAT) - 1), FRAME_FORMAT))
        error = "invalid_manifest";
      else
        error = frames.begin(id, frameText(command["sha256"], 64), now);
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
    reply["displaySupported"] = false;
    reply["displayRefreshedFrameId"] = "";
    return true;
  }
} // namespace avolyn
