#!/bin/sh
# Builds mario.wasm without a system clang: uses `zig cc` (pip install ziglang) as the wasm32 compiler/linker,
# then appends the manifest as the custom section "meos.manifest" (what llvm-objcopy --add-section does in tools/meos-build).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SDK=${SDK:-$HERE/../meos-sdk-src}
PY=${PY:-python3}
$PY -m ziglang cc --target=wasm32-freestanding -mcpu=mvp -nostdlib -O2 -s -ffreestanding -Wall -Wextra -Wno-unused-function \
    -I"$SDK/include" -Wl,--no-entry -Wl,--export=_start -Wl,-z,stack-size=8192 \
    -Wl,--initial-memory=65536 -Wl,--max-memory=16777216 -o "$HERE/mario.wasm" "$HERE/mario.c"
$PY -I - "$HERE/mario.wasm" "$HERE/mario.manifest" <<'PYEOF'
import sys
def leb(n):
    o = bytearray()
    while True:
        b = n & 0x7F; n >>= 7
        if n: o.append(b | 0x80)
        else: o.append(b); return bytes(o)
wasm, man = sys.argv[1], sys.argv[2]
d = open(wasm, "rb").read(); m = open(man, "rb").read()
name = b"meos.manifest"
payload = leb(len(name)) + name + m
open(wasm, "wb").write(d + b"\x00" + leb(len(payload)) + payload)
PYEOF
ls -l "$HERE/mario.wasm"
