#pragma once
#include <cerrno>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "avolyn_preset_store.h"

namespace avolyn {
  class PosixPresetFiles : public PresetFiles {
  public:
    explicit PosixPresetFiles(const char *root, void (*yield)() = nullptr)
        : root_(root), live_(root_ + "/catalog-v1.bin"), pending_(root_ + "/catalog-v1.pending"), yield_(yield) {}
    ~PosixPresetFiles() override {
      if (out_) std::fclose(out_);
    }
    bool prepare() {
      if (::mkdir(root_.c_str(), 0700) == 0) return true;
      struct stat st{};
      return errno == EEXIST && ::stat(root_.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    }
    long size(bool pending) override {
      struct stat st{};
      if (::stat(path(pending), &st) != 0) return errno == ENOENT ? -1 : -2;
      return S_ISREG(st.st_mode) ? static_cast<long>(st.st_size) : -2;
    }
    bool read(bool pending, size_t offset, uint8_t *bytes, size_t length) override {
      FILE *f = std::fopen(path(pending), "rb");
      if (!f) return false;
      const bool ok =
        std::fseek(f, static_cast<long>(offset), SEEK_SET) == 0 && std::fread(bytes, 1, length, f) == length;
      return std::fclose(f) == 0 && ok;
    }
    bool start() override {
      if (out_) return false;
      out_ = std::fopen(pending_.c_str(), "wb");
      return out_ != nullptr;
    }
    bool append(const uint8_t *bytes, size_t length) override {
      if (!out_ || !bytes) return false;
      for (size_t offset = 0; offset < length;) {
        const size_t count = length - offset < 1024 ? length - offset : 1024;
        if (std::fwrite(bytes + offset, 1, count, out_) != count) return false;
        offset += count;
        if (yield_) yield_();
      }
      return true;
    }
    bool finish() override {
      if (!out_) return false;
      const bool synced = std::fflush(out_) == 0 && ::fsync(::fileno(out_)) == 0;
      const bool closed = std::fclose(out_) == 0;
      out_ = nullptr;
      return synced && closed;
    }
    bool publish() override { return !out_ && std::rename(pending_.c_str(), live_.c_str()) == 0; }
    void cancel() override {
      if (out_) {
        std::fclose(out_);
        out_ = nullptr;
      }
      std::remove(pending_.c_str());
    }

  private:
    const char *path(bool pending) const { return pending ? pending_.c_str() : live_.c_str(); }
    std::string root_, live_, pending_;
    FILE *out_ = nullptr;
    void (*yield_)();
  };

  // Never format on an arbitrary mount error. Only a verified entirely erased
  // partition may be initialized automatically. Existing/corrupt data is preserved.
  template <typename Volume> bool mountPresetVolume(Volume &volume) {
    if (volume.mount()) return true;
    return volume.erased() && volume.format() && volume.mount();
  }
} // namespace avolyn
