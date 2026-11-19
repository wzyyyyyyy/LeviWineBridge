#!/usr/bin/env python3
"""Build the small Windows x64 preload client with the standard MinGW toolchain."""
import argparse
import os
import shlex
import shutil
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, default=source/'dist/LeviWineBridge.dll')
output = parser.parse_args().output
output.parent.mkdir(parents=True, exist_ok=True)
compiler = shlex.split(os.environ.get('MINGW_CC', 'x86_64-w64-mingw32-gcc'))
with tempfile.TemporaryDirectory() as scratch:
    artifact = Path(scratch)/'LeviWineBridge.dll'
    subprocess.run([*compiler, '-O2', '-Wall', '-Wextra', '-Werror',
                    '-shared', '-s', str(source/'client.c'),
                    '-lwinhttp', '-luser32', '-lws2_32', '-o', str(artifact)], check=True)
    shutil.copyfile(artifact, output)
print(output)
