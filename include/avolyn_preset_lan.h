#pragma once
// Portable authenticated preset endpoint. Caller holds the shared device mutex.
#include <cstdio>

#include "avolyn_frame_usb.h"
#include "avolyn_pairing_crypto.h"

namespace avolyn {
  constexpr char PRESET_LAN_PROTOCOL[] = "avolyn-presets-lan-v1";
  constexpr size_t PRESET_LAN_PLAIN = 2048, PRESET_LAN_WIRE = 4608;

  inline bool presetLanKey(const char *secret, const char *keyId, const char *context, bool response, uint8_t *key) {
    if (!isHex(keyId, 16) || !isHex(context, 16)) return false;
    uint8_t master[32], prk[32], expand[128] = {};
    if (!fromHex(secret, master, 32)) return false;
    const int length = std::snprintf(reinterpret_cast<char *>(expand), sizeof(expand), "%s/%s/%s", PRESET_LAN_PROTOCOL,
                                     context, response ? "response" : "request");
    const auto *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int result = -1;
    if (md && length > 0 && static_cast<size_t>(length + 1) <= sizeof(expand)) {
      expand[length] = 1;
      result = mbedtls_md_hmac(md, reinterpret_cast<const uint8_t *>(keyId), 32, master, 32, prk);
      if (!result) result = mbedtls_md_hmac(md, prk, 32, expand, length + 1, key);
    }
    wipe(master, sizeof(master));
    wipe(prk, sizeof(prk));
    wipe(expand, sizeof(expand));
    return result == 0;
  }
  inline bool presetLanDecrypt(const uint8_t *key, const char *ivText, const char *tagText, const char *cipherText,
                               const char *aad, uint8_t *plain, size_t &length) {
    length = 0;
    uint8_t iv[12], tag[16], cipher[PRESET_LAN_PLAIN];
    if (!cipherText || !*cipherText || std::strlen(cipherText) > sizeof(cipher) * 2 || std::strlen(cipherText) % 2 ||
        !fromHex(ivText, iv, 12) || !fromHex(tagText, tag, 16))
      return false;
    const size_t size = std::strlen(cipherText) / 2;
    if (!fromHex(cipherText, cipher, size)) return false;
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int result = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (!result)
      result = mbedtls_gcm_auth_decrypt(&gcm, size, iv, 12, reinterpret_cast<const uint8_t *>(aad), std::strlen(aad),
                                        tag, 16, cipher, plain);
    mbedtls_gcm_free(&gcm);
    if (result) {
      wipe(plain, size);
      return false;
    }
    plain[size] = 0;
    length = size;
    return true;
  }
  inline bool presetLanEncrypt(const uint8_t *key, const uint8_t *iv, const char *aad, const uint8_t *plain,
                               size_t size, char *cipherText, char *tagText) {
    if (!size || size > PRESET_LAN_PLAIN) return false;
    uint8_t cipher[PRESET_LAN_PLAIN], tag[16];
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int result = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (!result)
      result =
        mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, size, iv, 12, reinterpret_cast<const uint8_t *>(aad),
                                  std::strlen(aad), plain, cipher, 16, tag);
    mbedtls_gcm_free(&gcm);
    if (result) return false;
    toHex(cipher, size, cipherText);
    toHex(tag, 16, tagText);
    return true;
  }

  class PresetLan {
  public:
    Challenge challenge;
    void reset() {
      challenge.active = false;
      failed_ = false;
    }
  // Returns HTTP status; errors before authentication contain no device data.
    int process(const JsonDocument &envelope, const char *keyId, const char *secret, const char *mac,
                FrameReceiver &frames, PresetStore &store, uint32_t now, const uint8_t *responseIv, JsonDocument &out) {
      if (failed_ && uint32_t(now - lastFailed_) < 100) return 429;
      const char *context = frameText(envelope["contextId"], 32);
      const char *requestIv = frameText(envelope["iv"], 24);
      if (!challenge.valid(now) || !isHex(keyId, 16) || !isHex(secret, 32) || !envelope["v"].is<unsigned>() ||
          envelope["v"].as<unsigned>() != 1 || std::strcmp(frameText(envelope["keyId"], 32), keyId) ||
          !isHex(context, 16))
        return fail(now);
      char aad[256];
      std::snprintf(aad, sizeof(aad), "%s\nrequest\n%s\n%s\n%s", PRESET_LAN_PROTOCOL, keyId, challenge.value, context);
      uint8_t key[32];
      std::unique_ptr<uint8_t[]> plain(new (std::nothrow) uint8_t[PRESET_LAN_PLAIN + 1]);
      if (!plain || !presetLanKey(secret, keyId, context, false, key)) return 503;
      size_t length = 0;
      const auto ciphertext = envelope["ciphertext"].as<JsonString>();
      const bool validCipher = ciphertext.c_str() && ciphertext.size() == std::strlen(ciphertext.c_str());
      const bool authenticated = validCipher && presetLanDecrypt(key, requestIv, frameText(envelope["tag"], 32),
                                                                 ciphertext.c_str(), aad, plain.get(), length);
      wipe(key, sizeof(key));
      if (!authenticated) return fail(now);
      challenge.consume(challenge.value, now); // Authenticated malformed commands consume the token too.
      JsonDocument command, reply;
      const auto parsed = deserializeJson(command, reinterpret_cast<const char *>(plain.get()), length,
                                          DeserializationOption::NestingLimit(4));
      wipe(plain.get(), PRESET_LAN_PLAIN + 1);
      if (parsed || std::strcmp(frameText(command["challenge"], 64), challenge.value)) return 400;
      const char *cmd = command["cmd"] | "";
      const bool transfer = !std::strcmp(cmd, "FRAME_BEGIN") || !std::strcmp(cmd, "FRAME_CHUNK") ||
                            !std::strcmp(cmd, "FRAME_COMMIT") || !std::strcmp(cmd, "FRAME_CANCEL");
      const bool allowed = transfer || !std::strcmp(cmd, "FRAME_CAPS") || !std::strcmp(cmd, "FRAME_STATUS") ||
                           !std::strcmp(cmd, "PRESET_LIST") || !std::strcmp(cmd, "PRESET_SELECT") ||
                           !std::strcmp(cmd, "PRESET_DELETE");
      const char *error = nullptr;
      if (!allowed)
        error = "unsupported_command";
      else if (transfer && std::strcmp(frameText(command["transferId"], 32), context))
        error = "invalid_transfer_scope";
      else if (!std::strcmp(cmd, "FRAME_BEGIN") && (command["slot"].isNull() || command["expectedSlot"].isNull()))
        error = "invalid_expected_slot";
      if (!error)
        handleFrameUsb(command, frames, now, reply, 0, &store, true);
      else {
        reply["ok"] = false;
        reply["error"] = error;
        reply["cmd"] = cmd;
        reply["requestId"] = frameText(command["requestId"], 32);
      }
      reply["lanProtocol"] = PRESET_LAN_PROTOCOL;
      reply["macAddress"] = mac;
      if (measureJson(reply) > PRESET_LAN_PLAIN) return 503;
      const size_t size = serializeJson(reply, reinterpret_cast<char *>(plain.get()), PRESET_LAN_PLAIN + 1);
      if (!size || size > PRESET_LAN_PLAIN) return 503;
      std::snprintf(aad, sizeof(aad), "%s\nresponse\n%s\n%s\n%s\n%s", PRESET_LAN_PROTOCOL, keyId, challenge.value,
                    context, requestIv);
      std::unique_ptr<char[]> cipher(new (std::nothrow) char[PRESET_LAN_PLAIN * 2 + 1]);
      char tag[33], iv[25];
      if (!cipher || !presetLanKey(secret, keyId, context, true, key)) return 503;
      const bool encrypted = presetLanEncrypt(key, responseIv, aad, plain.get(), size, cipher.get(), tag);
      wipe(key, sizeof(key));
      wipe(plain.get(), PRESET_LAN_PLAIN + 1);
      if (!encrypted) return 503;
      toHex(responseIv, 12, iv);
      out["v"] = 1;
      out["keyId"] = keyId;
      out["contextId"] = context;
      out["iv"] = iv;
      out["ciphertext"] = cipher.get();
      out["tag"] = tag;
      return 200;
    }

  private:
    bool failed_ = false;
    uint32_t lastFailed_ = 0;
    int fail(uint32_t now) {
      failed_ = true;
      lastFailed_ = now;
      return 401;
    }
  };
} // namespace avolyn
