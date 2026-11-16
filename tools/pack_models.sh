#!/usr/bin/env bash
# Rebuild tools/srmodels/srmodels_cn.bin from tools/srmodels/src/.
#   src/wn9_nihaoxiaoan_tts2/  WakeNet9 "你好小安" (esp-sr model/wakenet_model)
#   src/mn7_cn/                MultiNet7 Chinese command recogniser
#   src/fst/commands_cn.txt    command table: "<id>,<pinyin>" (ids must match WAKE_CMD_* in wake_word.h)
#   src/vadnet1_medium/        VAD (kept for a future AFE pipeline)
# Flash with: tools/flash.sh --models
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
python3 -I "$HERE/pack_model.py" -m "$HERE/srmodels/src" -o srmodels.bin
mv "$HERE/srmodels/src/srmodels.bin" "$HERE/srmodels/srmodels_cn.bin"
ls -la "$HERE/srmodels/srmodels_cn.bin"
python3 -I - "$HERE/srmodels/srmodels_cn.bin" <<'PY'
import struct, sys
b = open(sys.argv[1], 'rb').read(); off = 0
def u32():
    global off; v = struct.unpack_from('<I', b, off)[0]; off += 4; return v
def cstr(n):
    global off; s = b[off:off+n].split(b'\0')[0].decode(); off += n; return s
n = u32(); print('models:', n)
for _ in range(n):
    name = cstr(32); k = u32(); files = []
    for _ in range(k): files.append((cstr(32), u32(), u32()))
    print(' ', name, ' '.join(f'{f}({l})' for f, _, l in files))
limit = 0x400000
print('size %d bytes, model partition %d bytes, %s' % (len(b), limit, 'OK' if len(b) <= limit else 'TOO BIG'))
PY
