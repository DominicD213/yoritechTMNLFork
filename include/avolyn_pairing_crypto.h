#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>

namespace avolyn {
  constexpr char PROTOCOL[] = "avolyn-pair-v1";
  constexpr size_t MAX_PLAINTEXT = 1024;
  inline bool isHex(const char *text, size_t bytes) {
    if (!text || std::strlen(text) != bytes * 2) return false;
    for (size_t i = 0; i < bytes * 2; ++i)
      if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return false;
    return true;
  }
  inline void wipe(void *data, size_t length) {
    volatile uint8_t *p = static_cast<volatile uint8_t *>(data);
    while (length--)
      *p++ = 0;
  }
  inline bool fromHex(const char *text, uint8_t *out, size_t bytes) {
    if (!isHex(text, bytes)) return false;
    auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (size_t i = 0; i < bytes; ++i)
      out[i] = (nibble(text[2 * i]) << 4) | nibble(text[2 * i + 1]);
    return true;
  }
  inline void toHex(const uint8_t *data, size_t length, char *out) {
    constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < length; ++i) {
      out[2 * i] = digits[data[i] >> 4];
      out[2 * i + 1] = digits[data[i] & 15];
    }
    out[2 * length] = 0;
  }
  inline bool deriveKey(const char *secret, const char *keyId, bool response, uint8_t *key) {
    uint8_t master[32];
    if (!isHex(keyId, 16) || !fromHex(secret, master, 32)) return false;
    // RFC 5869 HKDF-SHA256, one 32-byte output block. Arduino ESP32's
    // prebuilt Mbed TLS omits mbedtls_hkdf, but includes the HMAC primitive.
    const char *info = response ? "avolyn-pair-v1/response" : "avolyn-pair-v1/request";
    const auto *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    uint8_t prk[32], expand[64] = {};
    const size_t infoLength = std::strlen(info);
    std::memcpy(expand, info, infoLength);
    expand[infoLength] = 1;
    int result = mbedtls_md_hmac(md, reinterpret_cast<const uint8_t *>(keyId), 32, master, sizeof(master), prk);
    if (result == 0) result = mbedtls_md_hmac(md, prk, sizeof(prk), expand, infoLength + 1, key);
    wipe(master, sizeof(master));
    wipe(prk, sizeof(prk));
    wipe(expand, sizeof(expand));
    return result == 0;
  }
  inline bool decrypt(const uint8_t *key, const char *ivHex, const char *tagHex, const char *cipherHex, const char *aad,
                      uint8_t *plaintext, size_t &length) {
    uint8_t iv[12], tag[16], ciphertext[MAX_PLAINTEXT];
    length = 0;
    if (!cipherHex || std::strlen(cipherHex) > MAX_PLAINTEXT * 2 || std::strlen(cipherHex) % 2) return false;
    const size_t size = std::strlen(cipherHex) / 2;
    if (!size || !fromHex(ivHex, iv, sizeof(iv)) || !fromHex(tagHex, tag, sizeof(tag)) ||
        !fromHex(cipherHex, ciphertext, size))
      return false;
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    int result = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (result == 0)
      result = mbedtls_gcm_auth_decrypt(&ctx, size, iv, sizeof(iv), reinterpret_cast<const uint8_t *>(aad),
                                        std::strlen(aad), tag, sizeof(tag), ciphertext, plaintext);
    mbedtls_gcm_free(&ctx);
    if (result != 0) {
      wipe(plaintext, size);
      return false;
    }
    plaintext[size] = 0;
    length = size;
    return true;
  }
  inline bool encrypt(const uint8_t *key, const uint8_t *iv, const char *aad, const uint8_t *plaintext, size_t size,
                      char *cipherHex, char *tagHex) {
    if (!size || size > MAX_PLAINTEXT) return false;
    uint8_t ciphertext[MAX_PLAINTEXT], tag[16];
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    int result = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (result == 0)
      result =
        mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, size, iv, 12, reinterpret_cast<const uint8_t *>(aad),
                                  std::strlen(aad), plaintext, ciphertext, sizeof(tag), tag);
    mbedtls_gcm_free(&ctx);
    if (result != 0) return false;
    toHex(ciphertext, size, cipherHex);
    toHex(tag, sizeof(tag), tagHex);
    return true;
  }
// One fresh challenge per transaction; only successful authentication consumes it.
// Unsigned subtraction handles millis() wrap. Tokens are RAM-only and change on boot.
  struct Challenge {
    char value[65] = {};
    uint32_t issuedAt = 0;
    bool active = false;
    bool valid(uint32_t now) const { return active && uint32_t(now - issuedAt) < 10000; }
    void issue(const uint8_t *random, uint32_t now) {
      toHex(random, 32, value);
      issuedAt = now;
      active = true;
    }
    bool consume(const char *candidate, uint32_t now) {
      if (!valid(now) || !candidate || std::strcmp(candidate, value) != 0) return false;
      active = false;
      return true;
    }
  };
} // namespace avolyn
