#pragma once
// Portable, single-owner RAM receiver. No display, filesystem, NVS or network access.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

namespace avolyn {
  constexpr size_t FRAME_WIDTH = 792;
  constexpr size_t FRAME_HEIGHT = 272;
  constexpr size_t FRAME_STRIDE = 99;
  constexpr size_t FRAME_BYTES = FRAME_STRIDE * FRAME_HEIGHT;
  constexpr size_t FRAME_CHUNK_BYTES = 512;
  constexpr uint32_t FRAME_IDLE_MS = 15000;
  constexpr uint32_t FRAME_TOTAL_MS = 120000;

  inline bool frameHex(const char *s, size_t length) {
    if (!s || std::strlen(s) != length) return false;
    for (size_t i = 0; i < length; ++i)
      if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
  }

  struct PresetTarget {
    unsigned slot = 0; // Zero retains the original temporary RAM-upload contract.
    char presetId[33] = {};
    char name[65] = {};
  };
  inline bool presetNameValid(const char *name) {
    if (!name || !*name || std::strlen(name) > 64) return false;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(name); *p; ++p)
      if (*p < 32 || *p == 127) return false;
    return true;
  }

  class FrameReceiver {
  public:
    using Digest = bool (*)(const uint8_t *, size_t, char *);
    using Save = const char *(*)(void *, const PresetTarget &, const char *, const char *, const uint8_t *);
    explicit FrameReceiver(Digest digest, Save save = nullptr, void *context = nullptr)
        : digest_(digest), save_(save), context_(context) {}
    FrameReceiver(const FrameReceiver &) = delete;
    FrameReceiver &operator=(const FrameReceiver &) = delete;

    void expire(uint32_t now) {
      if (staging_ && (uint32_t(now - touched_) >= FRAME_IDLE_MS || uint32_t(now - started_) >= FRAME_TOTAL_MS))
        discard();
    }
    const char *begin(const char *id, const char *sha256, uint32_t now, const PresetTarget &target = PresetTarget()) {
      expire(now);
      if (!frameHex(id, 32) || !frameHex(sha256, 64)) return "invalid_manifest";
      if (target.slot && (target.slot > 3 || !frameHex(target.presetId, 32) || !presetNameValid(target.name)))
        return "invalid_preset";
      if (std::strcmp(id, acceptedId_) == 0) return "already_committed";
      if (staging_) {
        if (std::strcmp(id, stagingId_) || std::strcmp(sha256, stagingDigest_) || target.slot != target_.slot ||
            std::strcmp(target.presetId, target_.presetId) || std::strcmp(target.name, target_.name))
          return "busy";
        return nullptr; // A retry does not restart either deadline.
      }
      std::unique_ptr<uint8_t[]> next(new (std::nothrow) uint8_t[FRAME_BYTES]);
      if (!next) return "out_of_memory";
      staging_ = std::move(next);
      target_ = target;
      std::memcpy(stagingId_, id, 33);
      std::memcpy(stagingDigest_, sha256, 65);
      received_ = 0;
      started_ = touched_ = now;
      return nullptr;
    }
    const char *chunk(const char *id, size_t offset, const uint8_t *bytes, size_t length, uint32_t now) {
      expire(now);
      if (!staging_ || !id || std::strcmp(id, stagingId_)) return "no_transfer";
      if (!bytes || !length || length > FRAME_CHUNK_BYTES || offset > FRAME_BYTES || length > FRAME_BYTES - offset)
        return "invalid_chunk";
      if (offset < received_) {
        if (length > received_ - offset || std::memcmp(staging_.get() + offset, bytes, length))
          return "conflicting_chunk";
        return nullptr; // Identical retransmissions are safe; no timeout extension.
      }
      if (offset != received_) return "unexpected_offset";
      std::memcpy(staging_.get() + offset, bytes, length);
      received_ += length;
      touched_ = now;
      return nullptr;
    }
    const char *commit(const char *id, uint32_t now) {
      expire(now);
      if (!frameHex(id, 32)) return "invalid_transfer";
      if (std::strcmp(id, acceptedId_) == 0) return nullptr;
      if (!staging_ || std::strcmp(id, stagingId_)) return "no_transfer";
      if (received_ != FRAME_BYTES) return "incomplete";
      char actual[65] = {};
      if (!digest_ || !digest_(staging_.get(), FRAME_BYTES, actual)) {
        discard();
        return "digest_failed";
      }
      if (std::strcmp(actual, stagingDigest_)) {
        discard();
        return "checksum_mismatch";
      }
      if (target_.slot) {
        if (!save_) return "preset_storage_unavailable";
        const char *error = save_(context_, target_, stagingId_, stagingDigest_, staging_.get());
        if (error) return error; // Keep previous RAM/flash images until a durable save succeeds.
      }
      active_ = std::move(staging_); // Complete verified frame replaces the previous active image atomically.
      activeSlot_ = target_.slot;
      std::memcpy(acceptedId_, stagingId_, sizeof(acceptedId_));
      std::memcpy(acceptedDigest_, stagingDigest_, sizeof(acceptedDigest_));
      discard();
      return nullptr;
    }
    const char *cancel(const char *id) {
      if (!frameHex(id, 32)) return "invalid_transfer";
      if (staging_ && std::strcmp(id, stagingId_)) return "wrong_transfer";
      discard();
      return nullptr; // Cancelling or retrying after commit never erases active_.
    }
    size_t nextOffset() const { return received_; }
    const char *receivingId() const { return stagingId_; }
    const char *acceptedId() const { return acceptedId_; }
    const char *acceptedDigest() const { return acceptedDigest_; }
    const uint8_t *activePixels() const { return active_.get(); }
    unsigned activeSlot() const { return activeSlot_; }
    bool busy() const { return static_cast<bool>(staging_); }
    const char *installActive(std::unique_ptr<uint8_t[]> pixels, const char *id, const char *sha, unsigned slot) {
      if (busy() || !pixels || !frameHex(id, 32) || !frameHex(sha, 64) || slot < 1 || slot > 3) return "busy";
      active_ = std::move(pixels);
      std::memcpy(acceptedId_, id, 33);
      std::memcpy(acceptedDigest_, sha, 65);
      activeSlot_ = slot;
      return nullptr;
    }

  private:
    void discard() {
      staging_.reset();
      stagingId_[0] = stagingDigest_[0] = '\0';
      received_ = 0;
      target_ = PresetTarget();
    }
    Digest digest_;
    Save save_;
    void *context_;
    PresetTarget target_;
    unsigned activeSlot_ = 0;
    std::unique_ptr<uint8_t[]> staging_, active_;
    char stagingId_[33] = {}, stagingDigest_[65] = {};
    char acceptedId_[33] = {}, acceptedDigest_[65] = {};
    size_t received_ = 0;
    uint32_t started_ = 0, touched_ = 0;
  };
} // namespace avolyn
