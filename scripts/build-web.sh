#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
zig="${ZIG:-$root/_deps/zig/zig}"
engines_dir="${ENGINES_DIR:-$root/build}"
engines=("psp/EBOOT.PBP:psp" "vita/eboot.bin:vita" "vita/param.sfo:vita" "3ds/tcoaal.3dsx:3ds" "wii/tcoaal.dol:wii" "wiiu/tcoaal.rpx:wiiu"
  "ps2/tcoaal.elf:ps2" "ps3/EBOOT.BIN:ps3" "xenon/xenon.elf:xbox360")
blobs=(embedded_psp_eboot embedded_vita_eboot embedded_vita_sfo embedded_3ds_engine embedded_wii_dol embedded_wiiu_rpx embedded_ps2_elf
  embedded_ps3_eboot embedded_xenon_elf embedded_ps1_exe)
arts=("psp.png:embedded_psp_art" "vita.png:embedded_vita_art" "3ds.png:embedded_3ds_art" "wii.png:embedded_wii_art" "wiiu.png:embedded_wiiu_art"
  "ps2.png:embedded_ps2_art" "ds3.png:embedded_ps3_art" "x360.png:embedded_xenon_art")
cia="$engines_dir/3ds/tcoaal.cia"
src="$root/tools/builder"
out="$root/build/web"
gen="$root/build/web-gen"
for pair in "${engines[@]}"; do
  [ -f "$engines_dir/${pair%%:*}" ] || { echo "engine not found: $engines_dir/${pair%%:*} (run make engines first)"; exit 1; }
done
[ -f "$cia" ] || { echo "CIA template not found: $cia (run make 3ds first)"; exit 1; }
mkdir -p "$out" "$gen"
find "$out" -mindepth 1 -delete
rm -f "$gen"/embedded_*.c
cc -O2 -o "$gen/embed" "$src/tools/embed.c"
for sym in "${blobs[@]}"; do
  printf '#include <stddef.h>\nconst unsigned char %s[1] = {0};\nconst size_t %s_size = 0;\n' "$sym" "$sym" > "$gen/$sym.c"
done
"$gen/embed" "$cia" "$gen/embedded_3ds_cia.c" embedded_3ds_cia
for pair in "${arts[@]}"; do
  "$gen/embed" "$src/art/${pair%%:*}" "$gen/${pair##*:}.c" "${pair##*:}"
done
"$zig" cc -target wasm32-wasi -std=c99 -O2 -D_DEFAULT_SOURCE -DMINIZ_NO_STDIO -D_WASI_EMULATED_PROCESS_CLOCKS \
  -I"$src/src" -I"$src/third_party" "$src"/src/*.c "$src/third_party/miniz.c" "$gen"/embedded_*.c \
  -lwasi-emulated-process-clocks -Wl,--max-memory=0x80000000 -Wl,-z,stack-size=8388608 -s -o "$out/tcoaal-builder.wasm"
cp -r "$root"/web/. "$out/"
for pair in "${engines[@]}"; do
  mkdir -p "$out/engines/${pair##*:}"
  cp "$engines_dir/${pair%%:*}" "$out/engines/${pair##*:}/"
done
ls -la "$out" "$out"/engines/*
