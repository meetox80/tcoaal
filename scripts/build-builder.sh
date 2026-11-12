#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
zig="${ZIG:-$root/_deps/zig/zig}"
engines_dir="${ENGINES_DIR:-$root/build}"
engines=("psp/EBOOT.PBP:embedded_psp_eboot" "vita/eboot.bin:embedded_vita_eboot" "vita/param.sfo:embedded_vita_sfo"
  "3ds/tcoaal.3dsx:embedded_3ds_engine" "3ds/tcoaal.cia:embedded_3ds_cia" "wii/tcoaal.dol:embedded_wii_dol" "wiiu/tcoaal.rpx:embedded_wiiu_rpx"
  "ps2/tcoaal.elf:embedded_ps2_elf" "ps3/EBOOT.BIN:embedded_ps3_eboot" "xenon/xenon.elf:embedded_xenon_elf")
optional=("ps1/tcoaal.exe:embedded_ps1_exe")
arts=("psp.png:embedded_psp_art" "vita.png:embedded_vita_art" "3ds.png:embedded_3ds_art" "wii.png:embedded_wii_art" "wiiu.png:embedded_wiiu_art"
  "ps2.png:embedded_ps2_art" "ds3.png:embedded_ps3_art" "x360.png:embedded_xenon_art")
src="$root/tools/builder"
out="$root/build/builder"
gen="$out/gen"
mkdir -p "$gen"
rm -f "$gen"/embedded_*.c
for pair in "${engines[@]}"; do
  [ -f "$engines_dir/${pair%%:*}" ] || { echo "engine not found: $engines_dir/${pair%%:*} (run make engines first)"; exit 1; }
done
cc -O2 -o "$gen/embed" "$src/tools/embed.c"
for pair in "${engines[@]}"; do
  "$gen/embed" "$engines_dir/${pair%%:*}" "$gen/${pair##*:}.c" "${pair##*:}"
done
for pair in "${optional[@]}"; do
  if [ -f "$engines_dir/${pair%%:*}" ]; then
    "$gen/embed" "$engines_dir/${pair%%:*}" "$gen/${pair##*:}.c" "${pair##*:}"
  else
    printf '#include <stddef.h>\nconst unsigned char %s[1] = {0};\nconst size_t %s_size = 0;\n' "${pair##*:}" "${pair##*:}" > "$gen/${pair##*:}.c"
  fi
done
for pair in "${arts[@]}"; do
  "$gen/embed" "$src/art/${pair%%:*}" "$gen/${pair##*:}.c" "${pair##*:}"
done
sources=("$src"/src/*.c "$src/third_party/miniz.c" "$gen"/embedded_*.c)
targets=("${@:-x86_64-linux-musl aarch64-linux-musl x86_64-windows-gnu aarch64-windows-gnu x86_64-macos aarch64-macos}")
for t in ${targets[@]}; do
  name="tcoaal-builder-$t"
  libs="-lm"
  case "$t" in
    *windows*) name="$name.exe"; libs="-lole32 -lshell32 -luser32 -lgdi32 -ladvapi32 -luuid" ;;
    *linux*) libs="-lm -lpthread -static" ;;
  esac
  "$zig" cc -target "$t" -std=c99 -O2 -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64 -DMINIZ_NO_STDIO -I"$src/src" -I"$src/third_party" \
    -s "${sources[@]}" -o "$out/$name" $libs
  rm -f "$out/${name%.exe}.pdb"
  echo "built $out/$name"
done
