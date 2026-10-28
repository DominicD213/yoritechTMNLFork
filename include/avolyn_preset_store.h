#pragma once
#include <array>

#include "avolyn_frame_receiver.h"

namespace avolyn {
  constexpr unsigned PRESET_SLOTS = 3;
  constexpr size_t PRESET_HEADER_BYTES = 1024;
  constexpr size_t PRESET_FILE_BYTES = PRESET_HEADER_BYTES + PRESET_SLOTS * FRAME_BYTES;
  struct PresetRecord {
    bool occupied = false, valid = false;
    char frameId[33] = {}, presetId[33] = {}, name[65] = {}, sha256[65] = {}, actualSha256[65] = {};
  };
  // publish() atomically replaces the complete catalog. Failed writes never
  // publish the temporary file. The ESP32 adapter uses LittleFS + sync + rename.
  class PresetFiles {
  public:
    virtual ~PresetFiles() {}
    virtual long size(bool pending) = 0; // -1 = absent, -2 = I/O error.
    virtual bool read(bool pending, size_t offset, uint8_t *bytes, size_t length) = 0;
    virtual bool start() = 0;
    virtual bool append(const uint8_t *bytes, size_t length) = 0;
    virtual bool finish() = 0;
    virtual bool publish() = 0;
    virtual void cancel() = 0;
  };

  class PresetStore {
  public:
    PresetStore(PresetFiles &files, FrameReceiver::Digest digest) : files_(files), digest_(digest) {}
    bool ready() const { return ready_; }
    unsigned selectedSlot() const { return selected_; }
    const char *error() const { return error_; }
    const PresetRecord &slot(unsigned n) const { return records_[n - 1]; }
    bool initialize() {
      ready_ = false;
      records_ = {};
      selected_ = 0;
      const long length = files_.size(false);
      if (length == -1) {
        ready_ = true;
        error_ = "";
        return true;
      }
      std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[FRAME_BYTES]);
      if (!buffer) {
        error_ = "out_of_memory";
        return false;
      }
      if (length != static_cast<long>(PRESET_FILE_BYTES) || !files_.read(false, 0, buffer.get(), PRESET_HEADER_BYTES) ||
          !decode(buffer.get(), records_, selected_)) {
        error_ = "preset_catalog_invalid";
        return false;
      }
      for (unsigned i = 1; i <= PRESET_SLOTS; ++i)
        if (records_[i - 1].occupied) {
          auto &r = records_[i - 1];
          if (!files_.read(false, imageOffset(i), buffer.get(), FRAME_BYTES) ||
              !digest_(buffer.get(), FRAME_BYTES, r.actualSha256)) {
            error_ = "preset_read_failed";
            return false;
          }
          r.valid = std::strcmp(r.sha256, r.actualSha256) == 0;
        }
      ready_ = true;
      error_ = "";
      return true;
    }
    const char *save(const PresetTarget &target, const char *frameId, const char *sha, const uint8_t *pixels) {
      if (!ready_) return "preset_storage_unavailable";
      if (target.slot < 1 || target.slot > 3 || !frameHex(target.presetId, 32) || !presetNameValid(target.name) ||
          !frameHex(frameId, 32) || !frameHex(sha, 64) || !pixels)
        return "invalid_preset";
      char actual[65] = {};
      if (!digest_(pixels, FRAME_BYTES, actual) || std::strcmp(actual, sha)) return "checksum_mismatch";
      auto next = records_;
      auto &r = next[target.slot - 1];
      const bool identical = r.occupied && r.valid && !std::strcmp(r.presetId, target.presetId) &&
                             !std::strcmp(r.name, target.name) && !std::strcmp(r.sha256, sha);
      if (identical && selected_ == target.slot) return nullptr; // No repeated flash writes for unchanged data.
      if (!identical) {
        r = PresetRecord();
        r.occupied = r.valid = true;
        std::memcpy(r.frameId, frameId, 33);
        std::memcpy(r.presetId, target.presetId, 33);
        std::memcpy(r.name, target.name, std::strlen(target.name) + 1);
        std::memcpy(r.sha256, sha, 65);
        std::memcpy(r.actualSha256, sha, 65);
      }
      return write(next, target.slot, target.slot, pixels);
    }
    const char *select(unsigned n, const char *expectedSha) {
      if (!ready_) return "preset_storage_unavailable";
      if (n < 1 || n > 3 || !records_[n - 1].occupied || !records_[n - 1].valid) return "preset_not_available";
      if (!frameHex(expectedSha, 64) || std::strcmp(records_[n - 1].sha256, expectedSha)) return "preset_changed";
      if (n == selected_) return nullptr;
      return write(records_, n, 0, nullptr);
    }
    std::unique_ptr<uint8_t[]> load(unsigned n) {
      if (!ready_ || n < 1 || n > 3 || !records_[n - 1].occupied || !records_[n - 1].valid) return {};
      std::unique_ptr<uint8_t[]> bytes(new (std::nothrow) uint8_t[FRAME_BYTES]);
      char actual[65] = {};
      if (!bytes || !files_.read(false, imageOffset(n), bytes.get(), FRAME_BYTES) ||
          !digest_(bytes.get(), FRAME_BYTES, actual) || std::strcmp(actual, records_[n - 1].sha256))
        return {};
      return bytes;
    }

  private:
    static size_t imageOffset(unsigned n) { return PRESET_HEADER_BYTES + (n - 1) * FRAME_BYTES; }
    bool encode(uint8_t *h, const std::array<PresetRecord, 3> &records, unsigned selected) {
      std::memset(h, 0, PRESET_HEADER_BYTES);
      std::memcpy(h, "AVP57901", 8);
      h[8] = selected;
      h[9] = 3;
      for (unsigned i = 0; i < 3; ++i)
        if (records[i].occupied) {
          const auto &r = records[i];
          uint8_t *e = h + 16 + i * 208;
          e[0] = 1;
          e[1] = std::strlen(r.name);
          std::memcpy(e + 16, r.frameId, 32);
          std::memcpy(e + 48, r.presetId, 32);
          std::memcpy(e + 80, r.sha256, 64);
          std::memcpy(e + 144, r.name, e[1]);
        }
      char sha[65] = {};
      if (!digest_(h, 640, sha)) return false;
      std::memcpy(h + 640, sha, 64);
      return true;
    }
    bool decode(const uint8_t *h, std::array<PresetRecord, 3> &records, unsigned &selected) {
      char actual[65] = {}, expected[65] = {};
      std::memcpy(expected, h + 640, 64);
      if (std::memcmp(h, "AVP57901", 8) || h[9] != 3 || h[8] > 3 || !frameHex(expected, 64) ||
          !digest_(h, 640, actual) || std::strcmp(actual, expected))
        return false;
      for (size_t j = 10; j < 16; ++j)
        if (h[j]) return false;
      for (size_t j = 704; j < PRESET_HEADER_BYTES; ++j)
        if (h[j]) return false;
      records = {};
      for (unsigned i = 0; i < 3; ++i) {
        const uint8_t *e = h + 16 + i * 208;
        auto &r = records[i];
        if (!e[0]) {
          for (size_t j = 0; j < 208; ++j)
            if (e[j]) return false;
          continue;
        }
        if (e[0] != 1 || e[1] < 1 || e[1] > 64) return false;
        for (size_t j = 2; j < 16; ++j)
          if (e[j]) return false;
        for (size_t j = e[1]; j < 64; ++j)
          if (e[144 + j]) return false;
        r.occupied = true;
        std::memcpy(r.frameId, e + 16, 32);
        std::memcpy(r.presetId, e + 48, 32);
        std::memcpy(r.sha256, e + 80, 64);
        std::memcpy(r.name, e + 144, e[1]);
        if (!frameHex(r.frameId, 32) || !frameHex(r.presetId, 32) || !frameHex(r.sha256, 64) ||
            !presetNameValid(r.name) || std::strlen(r.name) != e[1])
          return false;
      }
      selected = h[8];
      return !selected || records[selected - 1].occupied;
    }
    const char *write(const std::array<PresetRecord, 3> &next, unsigned active, unsigned replacement,
                      const uint8_t *pixels) {
      std::unique_ptr<uint8_t[]> h(new (std::nothrow) uint8_t[PRESET_HEADER_BYTES]);
      std::unique_ptr<uint8_t[]> verify(new (std::nothrow) uint8_t[FRAME_BYTES]);
      if (!h || !verify) return "out_of_memory";
      if (!encode(h.get(), next, active) || !files_.start()) return "preset_write_failed";
      bool ok = files_.append(h.get(), PRESET_HEADER_BYTES);
      uint8_t block[512];
      for (unsigned n = 1; ok && n <= 3; ++n) {
        if (n == replacement) {
          ok = files_.append(pixels, FRAME_BYTES);
          continue;
        }
        for (size_t offset = 0; ok && offset < FRAME_BYTES; offset += sizeof(block)) {
          const size_t count = FRAME_BYTES - offset < sizeof(block) ? FRAME_BYTES - offset : sizeof(block);
          if (next[n - 1].occupied)
            ok = files_.read(false, imageOffset(n) + offset, block, count);
          else
            std::memset(block, 255, count);
          if (ok) ok = files_.append(block, count);
        }
      }
      ok = ok && files_.finish() && files_.size(true) == static_cast<long>(PRESET_FILE_BYTES) &&
           files_.read(true, 0, verify.get(), PRESET_HEADER_BYTES) &&
           !std::memcmp(h.get(), verify.get(), PRESET_HEADER_BYTES);
      for (unsigned n = 1; ok && n <= 3; ++n)
        if (next[n - 1].occupied) {
          char actual[65] = {};
          ok = files_.read(true, imageOffset(n), verify.get(), FRAME_BYTES) &&
               digest_(verify.get(), FRAME_BYTES, actual) && !std::strcmp(actual, next[n - 1].actualSha256);
        }
      if (!ok || !files_.publish()) {
        files_.cancel();
        return "preset_write_failed";
      }
      records_ = next;
      selected_ = active;
      error_ = "";
      return nullptr;
    }
    PresetFiles &files_;
    FrameReceiver::Digest digest_;
    std::array<PresetRecord, 3> records_{};
    unsigned selected_ = 0;
    bool ready_ = false;
    const char *error_ = "preset_storage_unavailable";
  };
} // namespace avolyn
