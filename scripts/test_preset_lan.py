#!/usr/bin/env python3
"""Actual C++ LAN handler/crypto/storage against Node wire messages, without hardware.
Optional AVOLYN_DESKTOP_ROOT runs the actual desktop client and transfer engine.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='avolyn-preset-lan-') as folder:
    exe=Path(folder)/'bridge'
    subprocess.run([os.environ.get('CXX','g++'),'-std=c++11','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-fno-omit-frame-pointer',*shlex.split(os.environ.get('CXXFLAGS','')),
        '-I'+str(root/'include'),'-I'+str(root/'.pio/libdeps/avolyn_headless/ArduinoJson/src'),
        str(root/'test/host_pairing/preset-lan-bridge.cpp'),'-lmbedcrypto','-o',str(exe)],check=True)
    subprocess.run(['node',str(root/'test/host_pairing/preset-lan-test.cjs'),str(exe),str(Path(folder)/'data')],check=True)
