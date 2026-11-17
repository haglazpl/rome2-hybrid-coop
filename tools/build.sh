#!/bin/sh
# Builds the two DLLs for 32-bit Windows with clang + lld (no Windows SDK needed):
#   out/hybrid_coop.dll  - the co-op logic (goes inside the mod pack, signed)
#   out/dinput8.dll      - the loader (players put it next to Rome2.exe)
# Needs: clang, lld (lld-link), llvm (llvm-dlltool), python3.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/out"; OBJ="$OUT/obj"
mkdir -p "$OBJ"
CC="${CC:-clang}"
DLLTOOL="${DLLTOOL:-llvm-dlltool}"
LINK="${LINK:-lld-link}"
CF="--target=i686-pc-windows-msvc -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-sse -mno-sse2 -mno-mmx \
 -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -Wall -Wextra -Wno-unused-parameter \
 -Wno-missing-field-initializers -I$ROOT/third_party/monocypher"
python3 "$ROOT/tools/gen_implibs.py" "$OBJ"
$DLLTOOL -m i386 -k -d "$OBJ/kernel32.def" -l "$OBJ/kernel32.lib"
$DLLTOOL -m i386 -k -d "$OBJ/user32.def" -l "$OBJ/user32.lib"
LF="/dll /nodefaultlib /entry:DllMain /machine:x86 /safeseh:no /opt:ref /opt:icf /Brepro"

$CC $CF -c "$ROOT/src/core/hook.c" -o "$OBJ/hook.o"
$CC $CF -c "$ROOT/src/core/coop.c" -o "$OBJ/coop.o"
$CC $CF -c "$ROOT/src/common/rt.c" -o "$OBJ/rt.o"
$LINK $LF /out:"$OUT/hybrid_coop.dll" "$OBJ/hook.o" "$OBJ/coop.o" "$OBJ/rt.o" "$OBJ/kernel32.lib" "$OBJ/user32.lib"

$CC $CF -w -c "$ROOT/third_party/monocypher/monocypher.c" -o "$OBJ/monocypher.o"
$CC $CF -w -c "$ROOT/third_party/monocypher/monocypher-ed25519.c" -o "$OBJ/monocypher-ed25519.o"
$CC $CF -c "$ROOT/src/loader/loader.c" -o "$OBJ/loader.o"
$LINK $LF /def:"$ROOT/src/loader/dinput8.def" /out:"$OUT/dinput8.dll" "$OBJ/loader.o" "$OBJ/rt.o" \
  "$OBJ/monocypher.o" "$OBJ/monocypher-ed25519.o" "$OBJ/kernel32.lib" "$OBJ/user32.lib"
rm -f "$OUT"/*.lib "$OUT"/*.exp
ls -l "$OUT"/*.dll
