// Isolated host test bridge; not part of any firmware image.
#include <cassert>
#include <iostream>
#include <string>

#include "avolyn_preset_files.h"
#include "avolyn_preset_lan.h"
struct Files : avolyn::PosixPresetFiles {
  std::string fault;
  unsigned writes = 0;
  explicit Files(const char *root) : avolyn::PosixPresetFiles(root) { assert(prepare()); }
  bool finish() override {
    const bool ok = avolyn::PosixPresetFiles::finish();
    return fault != "sync" && ok;
  }
  bool publish() override {
    if (fault == "rename") return false;
    bool ok = avolyn::PosixPresetFiles::publish();
    if (ok) ++writes;
    return ok;
  }
};
const char *save(void *context, const avolyn::PresetTarget &target, const char *id, const char *sha,
                 const uint8_t *pixels) {
  return static_cast<avolyn::PresetStore *>(context)->save(target, id, sha, pixels);
}
int main(int argc, char **argv) {
  assert(argc == 2);
  Files files(argv[1]);
  std::unique_ptr<avolyn::PresetStore> store;
  std::unique_ptr<avolyn::FrameReceiver> frames;
  avolyn::PresetLan lan;
  const char *keyId = "101112131415161718191a1b1c1d1e1f";
  const char *secret = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
  auto reboot = [&]() {
    frames.reset();
    store.reset(new avolyn::PresetStore(files, avolyn::frameSha256));
    assert(store->initialize());
    frames.reset(new avolyn::FrameReceiver(avolyn::frameSha256, save, store.get()));
    assert(!avolyn::restoreSavedPreset(*store, *frames));
    lan.reset();
  };
  reboot();
  std::string line;
  while (std::getline(std::cin, line)) {
    JsonDocument input, out;
    assert(!deserializeJson(input, line));
    const std::string action = input["action"] | "";
    const uint32_t now = input["now"].as<uint32_t>();
    if (action == "challenge") {
      if (!lan.challenge.valid(now)) {
        uint8_t random[32];
        assert(avolyn::fromHex(input["random"] | "", random, 32));
        lan.challenge.issue(random, now);
      }
      out["protocol"] = avolyn::PRESET_LAN_PROTOCOL;
      out["challenge"] = lan.challenge.value;
    } else if (action == "request") {
      uint8_t iv[12];
      assert(avolyn::fromHex(input["iv"] | "", iv, 12));
      JsonDocument envelope;
      envelope.set(input["envelope"]);
      JsonDocument reply;
      out["status"] = lan.process(envelope, keyId, secret, "68:EE:8F:50:5B:50", *frames, *store, now, iv, reply);
      out["body"] = reply;
    } else if (action == "usb") {
      JsonDocument command;
      command.set(input["command"]);
      assert(avolyn::handleFrameUsb(command, *frames, now, out, 2048, store.get()));
    } else if (action == "reboot") {
      reboot();
      out["ok"] = true;
    } else if (action == "fault") {
      files.fault = input["fault"] | "";
      out["ok"] = true;
    } else if (action == "state") {
      avolyn::presetList(out, *store);
      out["activePresetSlot"] = frames->activeSlot();
      out["selectedPresetSlot"] = store->selectedSlot();
      out["writes"] = files.writes;
    } else if (action == "reset-key") {
      lan.reset();
      if (frames->busy()) frames->cancel(frames->receivingId());
      out["ok"] = true;
    } else
      assert(false);
    std::string wire;
    serializeJson(out, wire);
    std::cout << wire << std::endl;
  }
}
