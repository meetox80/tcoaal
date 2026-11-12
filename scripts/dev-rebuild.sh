#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
target="${TARGET:-psp}"
[ -d "$root/build/desktop-$target" ] && cmake --build "$root/build/desktop-$target" -j"$(nproc)" 2>&1 | grep -E "error|warning:" || true
make -C "$root" --no-print-directory "$target" 2>&1 | grep -E "error|warning:" || true
[ -d "$root/build/builder-native" ] || cmake -S "$root/tools/builder" -B "$root/build/builder-native" >/dev/null
cmake --build "$root/build/builder-native" -j"$(nproc)" 2>&1 | grep -E "error|warning:" || true
case "$target" in
  3ds) rel="3ds/tcoaal/tcoaal.3dsx" engine="$root/build/3ds/tcoaal.3dsx" ;;
  ps2) rel="TCOAAL.iso" engine="$root/build/ps2/tcoaal.elf" ;;
  ps3) rel="TCOAAL.pkg" engine="$root/build/ps3/EBOOT.BIN" ;;
  *) rel="PSP/GAME/TCOAAL/EBOOT.PBP" engine="$root/build/psp/EBOOT.PBP" ;;
esac
for d in "$root/_work/dist" "$root/dist"; do
  [ -f "$d/$rel" ] && "$root/build/builder-native/tcoaal-builder" --update-engine "$d/$rel" --engine "$engine" >/dev/null
  [ "$target" = ps2 ] && [ -f "$d/usb/TCOAAL.ELF" ] && cp "$engine" "$d/usb/TCOAAL.ELF"
done
echo rebuilt
