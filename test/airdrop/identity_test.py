"""Decode the actual compiled discovery/Ask bytes for both supported boards."""
from pathlib import Path
import os
import plistlib
import shlex
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
subprocess.run([sys.executable, str(ROOT / 'scripts/generate_airdrop_identity.py')], check=True)
with tempfile.TemporaryDirectory(prefix='airdrop-identity-') as directory:
    root = Path(directory)
    source = root / 'identity.c'
    source.write_text('''
#include <stdio.h>
#include "vendor/esp32drop/src/airdrop/core/ad_identity.h"
int main(void) {
  unsigned char sizes[] = {sizeof(AD_DISC_BODY) >> 8, sizeof(AD_DISC_BODY) & 255,
                           sizeof(AD_ASK_BODY) >> 8, sizeof(AD_ASK_BODY) & 255};
  fwrite(sizes, 1, 4, stdout);
  fwrite(AD_DISC_BODY, 1, sizeof(AD_DISC_BODY), stdout);
  fwrite(AD_ASK_BODY, 1, sizeof(AD_ASK_BODY), stdout);
}
''')
    for classic, name in [(0, 'CrossPoint X4 Pro'), (1, 'CrossPoint X4C')]:
        exe = root / f'identity-{classic}'
        subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-I' + str(ROOT), f'-DFREEINK_DEVICE_X4CLASSIC={classic}', str(source), '-o', str(exe)], check=True)
        data = subprocess.check_output([str(exe)])
        disc_len, ask_len = struct.unpack('!HH', data[:4])
        assert len(data) == 4 + disc_len + ask_len
        expected = {'ReceiverComputerName': name, 'ReceiverModelName': 'M5Stack'}
        assert plistlib.loads(data[4 + disc_len:]) == expected
        expected['ReceiverMediaCapabilities'] = b'{"Version":1}'
        assert plistlib.loads(data[4:4 + disc_len]) == expected
print('Generated identity: both board names, plist types and fields passed')
