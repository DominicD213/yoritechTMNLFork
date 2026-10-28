#!/usr/bin/env python3
"""Host checks for actual preset storage, USB protocol and failure recovery. No hardware."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = r'''
#include "avolyn_frame_usb.h"
#include "avolyn_preset_files.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <vector>

struct FaultFiles : avolyn::PresetFiles {
  avolyn::PosixPresetFiles disk;
  std::string fault;
  unsigned publications = 0;
  explicit FaultFiles(const char *root) : disk(root) { assert(disk.prepare()); }
  long size(bool p) override { return disk.size(p); }
  bool read(bool p, size_t o, uint8_t *b, size_t n) override {
    if (!p && fault == "read") return false;
    const bool ok = disk.read(p,o,b,n);
    if (p && ok && n && fault == "verify") b[0] ^= 1;
    return ok;
  }
  bool start() override { return fault != "start" && disk.start(); }
  bool append(const uint8_t *b, size_t n) override {
    if (fault == "write") { disk.append(b,n < 64 ? n : 64); return false; }
    return disk.append(b,n);
  }
  bool finish() override { const bool ok=disk.finish(); return fault != "sync" && ok; }
  bool publish() override {
    if (fault == "rename") return false;
    const bool ok = disk.publish(); if (ok) ++publications; return ok;
  }
  void cancel() override { disk.cancel(); }
};
const char *save(void *context, const avolyn::PresetTarget &t, const char *id, const char *sha, const uint8_t *p) {
  return static_cast<avolyn::PresetStore *>(context)->save(t,id,sha,p);
}
std::string hexId(unsigned n) { char out[33]; std::snprintf(out,sizeof(out),"%032x",n); return out; }
std::string hash(const std::vector<uint8_t> &p) {
  char out[65] = {}; assert(avolyn::frameSha256(p.data(),p.size(),out)); return out;
}
JsonDocument send(avolyn::FrameReceiver &frames, avolyn::PresetStore &store, JsonDocument &request) {
  static unsigned sequence=10000;
  request["requestId"] = hexId(++sequence);
  std::string wire; serializeJson(request,wire); assert(wire.size() <= 1024);
  JsonDocument parsed, reply, owned;
  assert(!deserializeJson(parsed,wire));
  assert(avolyn::handleFrameUsb(parsed,frames,sequence,reply,2048,&store));
  std::string response; serializeJson(reply,response); assert(response.size() <= 2000);
  assert(!deserializeJson(owned,response));
  return owned;
}
JsonDocument command(avolyn::FrameReceiver &f, avolyn::PresetStore &s, const char *cmd,
                     const std::string &id="") {
  JsonDocument q; q["cmd"]=cmd; if (!id.empty()) q["transferId"]=id; return send(f,s,q);
}
std::string stage(avolyn::FrameReceiver &f, avolyn::PresetStore &s, unsigned slot,
                  const std::vector<uint8_t> &pixels, const std::string &name) {
  static unsigned transfer=1; const std::string id=hexId(++transfer);
  JsonDocument q; q["cmd"]="FRAME_BEGIN"; q["transferId"]=id;
  q["profile"]=avolyn::FRAME_PROFILE; q["pixelFormat"]=avolyn::FRAME_FORMAT;
  q["width"]=792; q["height"]=272; q["stride"]=99; q["bytes"]=avolyn::FRAME_BYTES;
  q["sha256"]=hash(pixels); q["slot"]=slot; q["presetId"]=hexId(slot);
  q["presetName"]=name; q["presetProtocol"]=avolyn::PRESET_PROTOCOL;
  assert(send(f,s,q)["ok"] == true);
  for (size_t offset=0; offset<pixels.size(); offset+=512) {
    const size_t count=pixels.size()-offset < 512 ? pixels.size()-offset : 512;
    unsigned char encoded[685]; size_t length=0;
    assert(mbedtls_base64_encode(encoded,sizeof(encoded),&length,pixels.data()+offset,count)==0);
    JsonDocument chunk; chunk["cmd"]="FRAME_CHUNK"; chunk["transferId"]=id;
    chunk["offset"]=offset; chunk["data"]=std::string(reinterpret_cast<char *>(encoded),length);
    const auto r=send(f,s,chunk); assert(r["ok"] == true); assert(r["nextOffset"].as<size_t>()==offset+count);
  }
  return id;
}
JsonDocument select(avolyn::FrameReceiver &f,avolyn::PresetStore &s,unsigned slot,const std::string &sha) {
  JsonDocument q; q["cmd"]="PRESET_SELECT"; q["slot"]=slot; q["sha256"]=sha; return send(f,s,q);
}
JsonDocument erase(avolyn::FrameReceiver &f,avolyn::PresetStore &s,unsigned slot,const char *id,const std::string &sha) {
  JsonDocument q; q["cmd"]="PRESET_DELETE"; q["slot"]=slot; q["frameId"]=id; q["sha256"]=sha; return send(f,s,q);
}
std::string fileBytes(const std::string &path) {
  std::ifstream in(path,std::ios::binary); return std::string(std::istreambuf_iterator<char>(in),{});
}
void writeBytes(const std::string &path,const std::string &bytes) {
  std::ofstream out(path,std::ios::binary|std::ios::trunc); out.write(bytes.data(),bytes.size()); assert(out.good());
}
struct Volume {
  bool existing=false, blank=false, formatted=false, formatOk=true;
  unsigned formatCalls=0, erasedCalls=0;
  bool mount() { return existing || formatted; }
  bool erased() { ++erasedCalls; return blank; }
  bool format() { ++formatCalls; formatted=formatOk; return formatOk; }
};
int main(int argc,char **argv) {
  if (argc==3 && std::string(argv[2])=="--bridge") {
    FaultFiles io(argv[1]); avolyn::PresetStore store(io,avolyn::frameSha256); assert(store.initialize());
    avolyn::FrameReceiver frames(avolyn::frameSha256,save,&store);
    assert(!avolyn::restoreSavedPreset(store,frames));
    std::string line;
    while (std::getline(std::cin,line)) {
      JsonDocument q,r; assert(!deserializeJson(q,line));
      static uint32_t now=0;
      assert(avolyn::handleFrameUsb(q,frames,++now,r,2048,&store));
      serializeJson(r,std::cout); std::cout << std::endl;
    }
    return 0;
  }
  assert(argc==2); const std::string root=argv[1], catalog=root+"/catalog-v1.bin", pending=root+"/catalog-v1.pending";
  Volume existing; existing.existing=true; assert(avolyn::mountPresetVolume(existing)); assert(!existing.formatCalls && !existing.erasedCalls);
  Volume blank; blank.blank=true; assert(avolyn::mountPresetVolume(blank)); assert(blank.formatCalls==1);
  Volume unreadable; assert(!avolyn::mountPresetVolume(unreadable)); assert(!unreadable.formatCalls);
  Volume failed; failed.blank=true; failed.formatOk=false; assert(!avolyn::mountPresetVolume(failed)); assert(failed.formatCalls==1);
  FaultFiles io(root.c_str()); avolyn::PresetStore store(io,avolyn::frameSha256); assert(store.initialize());
  avolyn::FrameReceiver frames(avolyn::frameSha256,save,&store);
  auto caps=command(frames,store,"FRAME_CAPS");
  assert(caps["maxPresets"]==3 && caps["presetStorageReady"]==true && caps["persistent"]==false);
  std::array<std::vector<uint8_t>,3> images;
  for (unsigned n=1;n<=3;++n) {
    images[n-1]=std::vector<uint8_t>(avolyn::FRAME_BYTES,static_cast<uint8_t>(30*n));
    const auto id=stage(frames,store,n,images[n-1],n==3?std::string(64,'"'):"Preset "+std::to_string(n));
    const auto done=command(frames,store,"FRAME_COMMIT",id);
    assert(done["ok"]==true && done["persistent"]==true && done["activePresetSlot"]==n);
    assert(done["sha256"].as<std::string>()==hash(images[n-1]));
    assert(command(frames,store,"FRAME_COMMIT",id)["ok"]==true);
  }
  assert(io.publications==3 && io.size(false)==static_cast<long>(avolyn::PRESET_FILE_BYTES));
  auto list=command(frames,store,"PRESET_LIST"); assert(list["slots"].size()==3);
  for (unsigned n=1;n<=3;++n) assert(list["slots"][n-1]["occupied"]==true && list["slots"][n-1]["valid"]==true);
  // A new receiver/store simulates restart. Every slot and the selected slot persist.
  avolyn::PresetStore boot(io,avolyn::frameSha256); assert(boot.initialize() && boot.selectedSlot()==3);
  avolyn::FrameReceiver rebooted(avolyn::frameSha256,save,&boot);
  assert(!avolyn::restoreSavedPreset(boot,rebooted));
  assert(io.publications==3 && rebooted.activeSlot()==3);
  for (unsigned n=1;n<=3;++n) { auto p=boot.load(n); assert(p && !std::memcmp(p.get(),images[n-1].data(),avolyn::FRAME_BYTES)); }
  assert(select(rebooted,boot,1,hash(images[0]))["ok"]==true); assert(boot.selectedSlot()==1 && rebooted.activeSlot()==1);
  const unsigned afterSelect=io.publications;
  assert(select(rebooted,boot,1,hash(images[0]))["ok"]==true); assert(io.publications==afterSelect);
  assert(select(rebooted,boot,0,hash(images[0]))["ok"]==false);
  assert(select(rebooted,boot,4,hash(images[0]))["ok"]==false);
  assert(select(rebooted,boot,2,std::string(64,'0'))["error"]=="preset_changed");
  const auto unchanged=stage(rebooted,boot,1,images[0],"Preset 1");
  assert(command(rebooted,boot,"FRAME_COMMIT",unchanged)["persistent"]==true);
  assert(io.publications==afterSelect); // Content/name/identity deduplication avoids flash writes.
  std::vector<uint8_t> replacement(avolyn::FRAME_BYTES,177);
  // Short write, failed sync, verification error and failed rename preserve every old slot and active RAM.
  for (const std::string fault : {"start","write","sync","verify","rename","read"}) {
    const auto before=fileBytes(catalog); const std::string oldId=rebooted.acceptedId();
    const auto id=stage(rebooted,boot,2,replacement,"Replacement");
    assert(select(rebooted,boot,1,hash(images[0]))["error"]=="busy");
    io.fault=fault; const auto rejected=command(rebooted,boot,"FRAME_COMMIT",id); io.fault="";
    assert(rejected["error"]=="preset_write_failed"); assert(fileBytes(catalog)==before);
    assert(oldId==rebooted.acceptedId() && rebooted.activeSlot()==1);
    assert(command(rebooted,boot,"FRAME_CANCEL",id)["ok"]==true);
    avolyn::PresetStore recovered(io,avolyn::frameSha256); assert(recovered.initialize() && recovered.selectedSlot()==1);
    for (unsigned n=1;n<=3;++n) assert(recovered.slot(n).valid);
  }
  // Leftover temporary data from an interrupted process never replaces the live catalog.
  const auto baseline=fileBytes(catalog); writeBytes(pending,std::string(400,'x'));
  avolyn::PresetStore afterCut(io,avolyn::frameSha256); assert(afterCut.initialize()); assert(fileBytes(catalog)==baseline);
  // A corrupt individual image is identified, other slots survive, and replacing that slot repairs it.
  std::string corrupt=baseline; corrupt[avolyn::PRESET_HEADER_BYTES]^=1; writeBytes(catalog,corrupt);
  avolyn::PresetStore damaged(io,avolyn::frameSha256); assert(damaged.initialize());
  assert(!damaged.slot(1).valid && damaged.slot(2).valid && damaged.slot(3).valid && !damaged.load(1));
  avolyn::FrameReceiver repaired(avolyn::frameSha256,save,&damaged);
  assert(!avolyn::restoreSavedPreset(damaged,repaired)); assert(repaired.activeSlot()==2);
  assert(select(repaired,damaged,1,hash(images[0]))["ok"]==false);
  const auto fixed=stage(repaired,damaged,1,replacement,"Repaired");
  assert(command(repaired,damaged,"FRAME_COMMIT",fixed)["persistent"]==true);
  avolyn::PresetStore verified(io,avolyn::frameSha256); assert(verified.initialize());
  for (unsigned n=1;n<=3;++n) assert(verified.slot(n).valid);
  // Damaged metadata fails closed and does not trigger formatting or overwrite existing files.
  const auto valid=fileBytes(catalog); corrupt=valid; corrupt[8]^=1; writeBytes(catalog,corrupt);
  avolyn::PresetStore invalid(io,avolyn::frameSha256); assert(!invalid.initialize());
  avolyn::PresetTarget target; target.slot=1; std::strcpy(target.presetId,hexId(1).c_str()); std::strcpy(target.name,"Test");
  assert(std::string(invalid.save(target,hexId(900).c_str(),hash(replacement).c_str(),replacement.data()))=="preset_storage_unavailable");
  assert(fileBytes(catalog)==corrupt); writeBytes(catalog,valid);
  // Deletion uses the same atomic catalog transaction, including failures and restarts.
  avolyn::PresetStore deleting(io,avolyn::frameSha256); assert(deleting.initialize());
  avolyn::FrameReceiver current(avolyn::frameSha256,save,&deleting);
  assert(!avolyn::restoreSavedPreset(deleting,current)); assert(current.activeSlot()==1);
  const std::string firstId=deleting.slot(1).frameId, firstSha=deleting.slot(1).sha256;
  assert(erase(current,deleting,1,hexId(999).c_str(),firstSha)["error"]=="preset_changed");
  for (const std::string fault : {"start","write","sync","verify","rename","read"}) {
    const auto before=fileBytes(catalog); io.fault=fault;
    assert(erase(current,deleting,1,firstId.c_str(),firstSha)["error"]=="preset_write_failed"); io.fault="";
    assert(fileBytes(catalog)==before && deleting.slot(1).occupied && current.activeSlot()==1);
  }
  auto staged=stage(current,deleting,2,replacement,"Pending");
  assert(erase(current,deleting,1,firstId.c_str(),firstSha)["error"]=="busy");
  assert(command(current,deleting,"FRAME_CANCEL",staged)["ok"]==true);
  const auto removed=erase(current,deleting,1,firstId.c_str(),firstSha);
  assert(removed["ok"]==true && removed["deletionPersistent"]==true && removed["deletedPresetSlot"]==1);
  assert(!deleting.slot(1).occupied && deleting.selectedSlot()==0 && !current.activePixels() && current.activeSlot()==0);
  avolyn::PresetStore deletedBoot(io,avolyn::frameSha256); assert(deletedBoot.initialize());
  avolyn::FrameReceiver emptyBoot(avolyn::frameSha256,save,&deletedBoot);
  assert(!avolyn::restoreSavedPreset(deletedBoot,emptyBoot) && emptyBoot.activeSlot()==0);
  for (unsigned n=2;n<=3;++n) { auto p=deletedBoot.load(n); assert(p && !std::memcmp(p.get(),images[n-1].data(),avolyn::FRAME_BYTES)); }
  // A corrupt slot can be deleted using its catalog identity without loading its damaged pixels.
  auto damagedBytes=fileBytes(catalog); damagedBytes[avolyn::PRESET_HEADER_BYTES+avolyn::FRAME_BYTES]^=1;
  writeBytes(catalog,damagedBytes);
  avolyn::PresetStore badImage(io,avolyn::frameSha256); assert(badImage.initialize() && !badImage.slot(2).valid);
  const std::string badId=badImage.slot(2).frameId, badSha=badImage.slot(2).sha256;
  assert(erase(emptyBoot,badImage,2,badId.c_str(),badSha)["ok"]==true);
  assert(select(emptyBoot,badImage,3,hash(images[2]))["ok"]==true);
  auto thirdId=std::string(badImage.slot(3).frameId);
  assert(erase(emptyBoot,badImage,3,thirdId.c_str(),hash(images[2]))["ok"]==true);
  avolyn::PresetStore allEmpty(io,avolyn::frameSha256); assert(allEmpty.initialize() && allEmpty.selectedSlot()==0);
  for (unsigned n=1;n<=3;++n) assert(!allEmpty.slot(n).occupied);
  assert(erase(emptyBoot,allEmpty,0,firstId.c_str(),firstSha)["ok"]==false);
  assert(erase(emptyBoot,allEmpty,4,firstId.c_str(),firstSha)["ok"]==false);
  avolyn::FrameReceiver reusable(avolyn::frameSha256,save,&allEmpty);
  auto reuse=stage(reusable,allEmpty,1,replacement,"Reused slot");
  assert(command(reusable,allEmpty,"FRAME_COMMIT",reuse)["persistent"]==true);
  std::cout << "Three-slot persistence, reboot/selection, atomic failure recovery, corruption, deduplication and USB checks passed.\n";
}
'''
with tempfile.TemporaryDirectory(prefix="avolyn-presets-") as folder:
    cpp = Path(folder) / "presets.cpp"
    exe = Path(folder) / "presets"
    cpp.write_text(source)
    subprocess.run([
        os.environ.get("CXX", "g++"), "-std=c++11", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        *shlex.split(os.environ.get("CXXFLAGS", "")), "-I" + str(root / "include"),
        "-I" + str(root / ".pio/libdeps/avolyn_headless/ArduinoJson/src"),
        str(cpp), "-lmbedcrypto", "-o", str(exe)
    ], check=True)
    subprocess.run([str(exe), str(Path(folder) / "data")], check=True)
