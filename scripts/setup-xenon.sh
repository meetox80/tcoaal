#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
libxenon_rev="7d75a30f81381f48fcaaa758290a2b9d144a8fd1"
export DEVKITXENON="$root/_deps/xenon"
export PATH="$DEVKITXENON/bin:$DEVKITXENON/usr/bin:$PATH"
portlibs="$DEVKITXENON/usr"
jobs="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

git clone -q https://github.com/Free60Project/libxenon "$tmp/libxenon"
git -C "$tmp/libxenon" checkout -q "$libxenon_rev"
build() { (cd "$tmp/libxenon/toolchain" && FREE60_DOCKER_BUILD=true PREFIX="$DEVKITXENON" PARALLEL="-j$jobs" ./build-xenon-toolchain "$1") || { tail -40 "$tmp/libxenon/toolchain/build.log"; exit 1; }; }

if [ ! -x "$DEVKITXENON/bin/xenon-g++" ]; then
  mkdir -p "$DEVKITXENON"
  build toolchain
fi
if [ ! -f "$portlibs/lib/libxenon.a" ]; then
  build libxenon
fi

sha256() {
  if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi
}
fetch() {
  curl -fsSL -o "$tmp/$2" "$1"
  if [ "$(sha256 "$tmp/$2")" != "$3" ]; then
    echo "checksum mismatch for $1"
    exit 1
  fi
  tar -xf "$tmp/$2" -C "$tmp"
}
export CFLAGS="-O2 -mcpu=cell -mtune=cell -m32 -fno-pic -mpowerpc64"
cross=(--host=ppc-elf --prefix="$portlibs" --disable-shared --enable-static)
export LDFLAGS="$portlibs/lib/libxenon.a -L$DEVKITXENON/xenon/lib/32 -T$DEVKITXENON/app.lds -u read -u _start -u exc_base"

if [ ! -f "$portlibs/lib/libz.a" ]; then
  fetch https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz zlib.tar.gz 9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
  (cd "$tmp/zlib-1.3.1" && CHOST=xenon CC=xenon-gcc AR=xenon-ar RANLIB=xenon-ranlib ./configure --prefix="$portlibs" --static && make -j"$jobs" libz.a && make install) >"$tmp/zlib.log" 2>&1 || { tail -40 "$tmp/zlib.log"; exit 1; }
fi
if [ ! -f "$portlibs/lib/libogg.a" ]; then
  fetch https://downloads.xiph.org/releases/ogg/libogg-1.3.5.tar.gz ogg.tar.gz 0eb4b4b9420a0f51db142ba3f9c64b333f826532dc0f48c6410ae51f4799b664
  (cd "$tmp/libogg-1.3.5" && CC=xenon-gcc AR=xenon-ar RANLIB=xenon-ranlib ./configure "${cross[@]}" && make -j"$jobs" && make install) >"$tmp/ogg.log" 2>&1 || { tail -40 "$tmp/ogg.log"; exit 1; }
fi
if [ ! -f "$portlibs/lib/libvorbisfile.a" ]; then
  fetch https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.gz vorbis.tar.gz 0e982409a9c3fc82ee06e08205b1355e5c6aa4c36bca58146ef399621b0ce5ab
  (cd "$tmp/libvorbis-1.3.7" && CC=xenon-gcc AR=xenon-ar RANLIB=xenon-ranlib OGG_CFLAGS="-I$portlibs/include" OGG_LIBS="-L$portlibs/lib -logg" ./configure "${cross[@]}" --disable-oggtest --disable-docs --disable-examples && make -j"$jobs" && make install) >"$tmp/vorbis.log" 2>&1 || { tail -40 "$tmp/vorbis.log"; exit 1; }
fi
if [ ! -f "$portlibs/lib/libfat.a" ]; then
  git clone -q --depth 1 https://github.com/Free60Project/fat-xenon "$tmp/fat-xenon"
  (make -C "$tmp/fat-xenon" && make -C "$tmp/fat-xenon" install) >"$tmp/fat.log" 2>&1 || { tail -40 "$tmp/fat.log"; exit 1; }
fi
"$DEVKITXENON/bin/xenon-gcc" --version | head -1
