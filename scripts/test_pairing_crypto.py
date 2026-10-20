#!/usr/bin/env python3
"""Compile the actual firmware crypto header against host Mbed TLS and test wire vectors.

Requires a C++ compiler, Mbed TLS 2.28+ headers, and libmbedcrypto.
Set CXXFLAGS/LDFLAGS when headers/libraries are outside the system search paths.
No board connection or firmware flashing is performed.
"""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
v = json.loads((root / 'test/host_pairing/pairing-v1.json').read_text())
# Generate only public test constants; production credentials never enter this test.
constants = {k: v[k] for k in ('secret', 'keyId', 'challenge', 'requestKey', 'responseKey', 'requestPlain', 'responsePlain')}
for direction in ('request', 'response'):
    for field in ('iv', 'ciphertext', 'tag'):
        constants[direction + field.title()] = v[direction][field]
source = '#include "avolyn_pairing_crypto.h"\n#include <cassert>\n#include <iostream>\n#include <string>\n'
source += '\n'.join('const char *' + k + ' = ' + json.dumps(value) + ';' for k, value in constants.items())
source += r'''
int main() {
  uint8_t key[32], out[avolyn::MAX_PLAINTEXT + 1], iv[12], random[32] = {};
  char keyHex[65], cipherHex[avolyn::MAX_PLAINTEXT * 2 + 1], tagHex[33];
  size_t length = 0;
  std::string aad = std::string(avolyn::PROTOCOL) + "\nrequest\n" + keyId + "\n" + challenge;
  assert(avolyn::deriveKey(secret, keyId, false, key));
  avolyn::toHex(key, sizeof(key), keyHex);
  assert(std::strcmp(keyHex, requestKey) == 0);
  assert(avolyn::decrypt(key, requestIv, requestTag, requestCiphertext, aad.c_str(), out, length));
  assert(std::string(reinterpret_cast<char *>(out), length) == requestPlain);
  assert(!avolyn::decrypt(key, requestIv, "00", requestCiphertext, aad.c_str(), out, length));
  assert(!avolyn::decrypt(key, requestIv, "00000000000000000000000000000000", requestCiphertext, aad.c_str(), out, length));
  assert(length == 0);
  assert(!avolyn::decrypt(key, requestIv, requestTag, "ff", aad.c_str(), out, length));
  assert(!avolyn::decrypt(key, requestIv, requestTag, requestCiphertext, "wrong-aad", out, length));
  assert(avolyn::deriveKey(secret, keyId, true, key));
  avolyn::toHex(key, sizeof(key), keyHex);
  assert(std::strcmp(keyHex, responseKey) == 0);
  // A request cannot be reflected as a response (different derived keys).
  assert(!avolyn::decrypt(key, requestIv, requestTag, requestCiphertext, aad.c_str(), out, length));
  aad = std::string(avolyn::PROTOCOL) + "\nresponse\n" + keyId + "\n" + challenge + "\n" + requestIv;
  assert(avolyn::fromHex(responseIv, iv, sizeof(iv)));
  assert(avolyn::encrypt(key, iv, aad.c_str(), reinterpret_cast<const uint8_t *>(responsePlain), std::strlen(responsePlain), cipherHex, tagHex));
  assert(std::strcmp(cipherHex, responseCiphertext) == 0);
  assert(std::strcmp(tagHex, responseTag) == 0);
  assert(avolyn::decrypt(key, responseIv, responseTag, responseCiphertext, aad.c_str(), out, length));
  assert(std::string(reinterpret_cast<char *>(out), length) == responsePlain);
  assert(!avolyn::fromHex("g0", out, 1));
  assert(!avolyn::fromHex("00ff", out, 1));
  assert(!avolyn::deriveKey("short", keyId, true, key));
  avolyn::Challenge token;
  assert(!token.valid(0)); // A boot does not resurrect an old challenge.
  token.issue(random, 100);
  assert(!token.consume("wrong", 101));
  assert(token.consume(token.value, 102));
  assert(!token.consume(token.value, 103)); // Replay.
  token.issue(random, 100);
  assert(!token.consume(token.value, 10100)); // Expired.
  token.issue(random, UINT32_MAX - 10);
  assert(token.consume(token.value, 20)); // Timer wrap.
  avolyn::wipe(key, sizeof(key));
  std::cout << "Firmware HKDF/GCM vectors, tampering, reflection, replay, expiry and timer wrap passed.\n";
}
'''
with tempfile.TemporaryDirectory(prefix='avolyn-crypto-') as temporary:
    cpp = Path(temporary) / 'pairing.cpp'
    exe = Path(temporary) / 'pairing'
    cpp.write_text(source)
    subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++11', '-Wall', '-Wextra', '-Werror',
                    *shlex.split(os.environ.get('CXXFLAGS', '')), '-I' + str(root / 'include'),
                    str(cpp), *shlex.split(os.environ.get('LDFLAGS', '')), '-lmbedcrypto', '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
