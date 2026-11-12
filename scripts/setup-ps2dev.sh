#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$root/_deps"
cd "$root/_deps"
case "$(uname -s)-$(uname -m)" in
  Darwin-x86_64) asset="ps2dev-macos-15-intel.tar.gz" ;;
  Darwin-*) asset="ps2dev-macos-latest.tar.gz" ;;
  Linux-aarch64) asset="ps2dev-ubuntu-24.04-arm.tar.gz" ;;
  *) asset="ps2dev-ubuntu-latest.tar.gz" ;;
esac
if [ ! -x ps2dev/ee/bin/mips64r5900el-ps2-elf-gcc ]; then
  curl -fL -o ps2dev.tar.gz "https://github.com/ps2dev/ps2dev/releases/latest/download/$asset"
  tar xzf ps2dev.tar.gz
  rm -f ps2dev.tar.gz
fi
"$root/_deps/ps2dev/ee/bin/mips64r5900el-ps2-elf-gcc" --version | head -1
