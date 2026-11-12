#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
makerom_tag=makerom-v0.19.0
bannertool_tag=1.2.3
out="$root/_deps/ctrtools/$(uname -s)-$(uname -m)"
if [ -x "$out/makerom" ] && [ -x "$out/bannertool" ]; then
  echo "$out"
  exit 0
fi
work="$root/_deps/ctrtools/src"
mkdir -p "$out" "$work"
cd "$work"
if [ ! -x "$out/makerom" ]; then
  curl -fsSL -o makerom.tar.gz "https://github.com/3DSGuy/Project_CTR/archive/refs/tags/$makerom_tag.tar.gz"
  tar xzf makerom.tar.gz
  { make -C "Project_CTR-$makerom_tag/makerom" deps && make -C "Project_CTR-$makerom_tag/makerom"; } >makerom.log 2>&1 || { tail -30 makerom.log >&2; exit 1; }
  cp "Project_CTR-$makerom_tag/makerom/bin/makerom" "$out/makerom"
fi
if [ ! -x "$out/bannertool" ]; then
  curl -fsSL -o bannertool.tar.gz "https://github.com/carstene1ns/3ds-bannertool/releases/download/$bannertool_tag/bannertool-$bannertool_tag.tar.gz"
  tar xzf bannertool.tar.gz
  { cmake -S "bannertool-$bannertool_tag" -B "bannertool-$bannertool_tag/build" -DCMAKE_BUILD_TYPE=Release &&
    cmake --build "bannertool-$bannertool_tag/build" -j"$(nproc 2>/dev/null || echo 4)"; } >bannertool.log 2>&1 || { tail -30 bannertool.log >&2; exit 1; }
  cp "bannertool-$bannertool_tag/build/bannertool" "$out/bannertool"
fi
cd "$root"
rm -rf "$work"
echo "$out"
