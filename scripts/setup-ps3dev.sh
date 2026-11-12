#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
deps="${DEPS:-$root/_deps}"
toolchain="${PS3TOOLCHAIN_TAG:-nightly-2026-10-11}"
psl1ght=f649a08fd536a9e27c08c7db2d93a2d7ee4c3bbe
zlib=zlib-1.3.1
ogg=libogg-1.3.5
vorbis=libvorbis-1.3.7
jobs="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
case "$(uname -s)-$(uname -m)" in
  Linux-x86_64) asset="ps3dev-linux-X64.tar.gz" ;;
  Darwin-arm64) asset="ps3dev-macos-ARM64.tar.gz" ;;
  Darwin-x86_64) asset="ps3dev-macos-X64.tar.gz" ;;
  *) echo "no prebuilt PS3 toolchain for $(uname -s) $(uname -m); build ps3toolchain yourself and pass PS3DEV=/path/to/ps3dev"; exit 1 ;;
esac

export PS3DEV="$deps/ps3dev"
export PSL1GHT="$PS3DEV"
export PATH="$PS3DEV/bin:$PS3DEV/ppu/bin:$PS3DEV/spu/bin:$PATH"
portlibs="$PS3DEV/portlibs/ppu"

sha256() {
  if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

fetch() {
  local url="$1" out="$2" sum="$3"
  [ -f "$out" ] || curl -fL -o "$out" "$url"
  if [ "$(sha256 "$out")" != "$sum" ]; then
    echo "checksum mismatch for $out"
    rm -f "$out"
    exit 1
  fi
}

mkdir -p "$deps/src"
cd "$deps"

if [ ! -x "$PS3DEV/ppu/bin/ppu-gcc" ]; then
  curl -fL -o ps3dev.tar.gz "https://github.com/ps3dev/ps3toolchain/releases/download/$toolchain/$asset"
  tar xzf ps3dev.tar.gz
  rm -rf ps3dev.tar.gz ps3dev/test.tmp
fi

if [ ! -f "$PS3DEV/ppu/lib/librsx.a" ] || [ ! -x "$PS3DEV/bin/make_self_npdrm" ]; then
  curl -fL -o src/psl1ght.tar.gz "https://github.com/ps3dev/PSL1GHT/archive/$psl1ght.tar.gz"
  rm -rf "src/PSL1GHT-$psl1ght"
  tar xzf src/psl1ght.tar.gz -C src
  make -C "src/PSL1GHT-$psl1ght" install-ctrl
  make -C "src/PSL1GHT-$psl1ght" -j"$jobs"
  make -C "src/PSL1GHT-$psl1ght" install
fi

cflags="-O2 -mcpu=cell -I$portlibs/include"

if [ ! -f "$portlibs/lib/libz.a" ]; then
  fetch "https://github.com/madler/zlib/releases/download/v1.3.1/$zlib.tar.gz" "src/$zlib.tar.gz" 9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
  rm -rf "src/$zlib" && tar xzf "src/$zlib.tar.gz" -C src
  (cd "src/$zlib" && CC=powerpc64-ps3-elf-gcc AR=powerpc64-ps3-elf-ar RANLIB=powerpc64-ps3-elf-ranlib CFLAGS="$cflags" \
    ./configure --prefix="$portlibs" --static && make -j"$jobs" libz.a && make install)
fi

build_lib() {
  local name="$1"
  rm -rf "src/$name" && tar xzf "src/$name.tar.gz" -C src
  (cd "src/$name" && CFLAGS="$cflags" LDFLAGS="-L$portlibs/lib" \
    ./configure --host=powerpc64-ps3-elf --prefix="$portlibs" --disable-shared --enable-static --disable-docs --disable-examples &&
    make -j"$jobs" && make install)
}

if [ ! -f "$portlibs/lib/libogg.a" ]; then
  fetch "https://downloads.xiph.org/releases/ogg/$ogg.tar.gz" "src/$ogg.tar.gz" 0eb4b4b9420a0f51db142ba3f9c64b333f826532dc0f48c6410ae51f4799b664
  build_lib "$ogg"
fi

if [ ! -f "$portlibs/lib/libvorbisfile.a" ]; then
  fetch "https://downloads.xiph.org/releases/vorbis/$vorbis.tar.gz" "src/$vorbis.tar.gz" 0e982409a9c3fc82ee06e08205b1355e5c6aa4c36bca58146ef399621b0ce5ab
  build_lib "$vorbis"
fi

"$PS3DEV/ppu/bin/ppu-gcc" --version | head -1
