PSPDEV ?= $(CURDIR)/_deps/pspdev
VITASDK ?= $(CURDIR)/_deps/vitasdk
DEVKITPRO ?= $(CURDIR)/_deps/devkitpro
PS2DEV ?= $(CURDIR)/_deps/ps2dev
PS3DEV ?= $(CURDIR)/_deps/ps3dev
DEVKITXENON ?= $(CURDIR)/_deps/xenon
VITA3K ?= $(CURDIR)/_deps/vita3k/Vita3K
ZIG ?= $(CURDIR)/_deps/zig/zig
JOBS ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
OUT ?= out
GAME ?=
VOICES ?= voices
TARGET ?= psp

export PSPDEV
export VITASDK
export DEVKITPRO
export DEVKITARM := $(DEVKITPRO)/devkitARM
export DEVKITPPC := $(DEVKITPRO)/devkitPPC
export PS2DEV
export PS2SDK := $(PS2DEV)/ps2sdk
export GSKIT := $(PS2DEV)/gsKit
export PS3DEV
export PSL1GHT := $(PS3DEV)
export DEVKITXENON
export PATH := $(PSPDEV)/bin:$(VITASDK)/bin:$(DEVKITARM)/bin:$(DEVKITPPC)/bin:$(DEVKITPRO)/tools/bin:$(PS2DEV)/bin:$(PS2DEV)/ee/bin:$(PS2DEV)/iop/bin:$(PS2DEV)/dvp/bin:$(PS2SDK)/bin:$(PS3DEV)/bin:$(PS3DEV)/ppu/bin:$(PATH)

BUILDER := build/builder-native/tcoaal-builder
CONSOLES := psp vita 3ds wii wiiu ps3 ps2 xbox360

PLAY_psp := PSP/GAME/TCOAAL/EBOOT.PBP
PLAY_vita := data/TCOAAL/tcoaal.pak
PLAY_3ds := 3ds/tcoaal/tcoaal.3dsx
PLAY_wii := apps/tcoaal/tcoaal.pak
PLAY_wiiu := wiiu/apps/tcoaal/tcoaal.pak
PLAY_ps2 := TCOAAL.iso
PLAY_ps3 := TCOAAL.PAK
PLAY_xbox360 := tcoaal/tcoaal.pak
PROFILE_xbox360 := xenon
RUN_ps3 := --pak "$(OUT)/TCOAAL.PAK"

.PHONY: all help engines $(CONSOLES) psp-toolchain vita-toolchain 3ds-toolchain wii-toolchain wiiu-toolchain ps2-toolchain ps3-toolchain \
	xbox360-toolchain ctrtools vita3k zig vita-shaders wiiu-shaders xbox360-shaders desktop builder test test-psp1000 run play release web web-serve clean

all: builder

help:
	@echo "make                  build every console engine and the builder for this machine"
	@echo "make engines          build every console engine"
	@echo "make psp              build the PSP engine (build/psp/EBOOT.PBP)"
	@echo "make vita             build the PS Vita engine (build/vita/eboot.bin and param.sfo)"
	@echo "make 3ds              build the 3DS engine (build/3ds/tcoaal.3dsx and the tcoaal.cia template)"
	@echo "make wii              build the Wii engine (build/wii/tcoaal.dol)"
	@echo "make wiiu             build the Wii U engine (build/wiiu/tcoaal.rpx)"
	@echo "make ps3              build the PS3 engine (build/ps3/EBOOT.BIN)"
	@echo "make ps2              build the PS2 engine (build/ps2/tcoaal.elf)"
	@echo "make xbox360          build the Xbox 360 engine (build/xenon/xenon.elf)"
	@echo "make psp-toolchain    download the pspdev toolchain into _deps/pspdev"
	@echo "make vita-toolchain   download vitasdk with zlib, libogg and libvorbis into _deps/vitasdk"
	@echo "make 3ds-toolchain    download devkitARM, libctru, citro3d and the 3DS portlibs into _deps/devkitpro"
	@echo "make wii-toolchain    download devkitPPC, libogc, libfat and the Wii portlibs into _deps/devkitpro"
	@echo "make wiiu-toolchain   download devkitPPC, wut and the Wii U portlibs into _deps/devkitpro"
	@echo "make ps2-toolchain    download the ps2dev toolchain into _deps/ps2dev"
	@echo "make ps3-toolchain    download the PS3 toolchain and build PSL1GHT, zlib, libogg and libvorbis into _deps/ps3dev"
	@echo "make xbox360-toolchain build the libxenon toolchain, zlib, libogg, libvorbis and libfat into _deps/xenon"
	@echo "make ctrtools         build makerom and bannertool into _deps/ctrtools (used by make 3ds for the CIA)"
	@echo "make vita3k           download the Vita3K emulator into _deps/vita3k (used by make vita-shaders)"
	@echo "make vita-shaders     recompile the Vita shaders in engine/src/platform/vita/shaders"
	@echo "make wiiu-shaders     assemble the Wii U shaders in engine/src/platform/wiiu/shaders (builds latte-assembler into _deps/decaf)"
	@echo "make xbox360-shaders  assemble the Xbox 360 shaders in engine/src/platform/xenon/shaders"
	@echo "make zig              download zig into _deps/zig (used by make release and make web)"
	@echo "make desktop          build the desktop test build (needs SDL2, libvorbis, zlib); TARGET picks the console it reproduces"
	@echo "make builder          build the builder for this machine ($(BUILDER))"
	@echo "make run              run the builder; GAME=path/to/game, OUT=folder, TARGET=$(subst $() $(),|,$(CONSOLES)) and VOICES=clip folder are optional"
	@echo "make play             run the built game in the desktop build (OUT and TARGET as used by make run)"
	@echo "make test             run the builder unit tests"
	@echo "make test-psp1000     play scripted scenes of a built EBOOT in PPSSPP as a PSP-1000 and check that every image loads; EBOOT=path is optional"
	@echo "make release          cross-compile the builder for Linux, Windows and macOS into build/builder"
	@echo "make web              build the in-browser builder page into build/web"
	@echo "make web-serve        serve build/web on http://localhost:8080"
	@echo "make clean            remove build/"

psp-toolchain: $(PSPDEV)/bin/psp-gcc

$(PSPDEV)/bin/psp-gcc:
	./scripts/setup-toolchain.sh

vita-toolchain: $(VITASDK)/arm-vita-eabi/lib/libvorbisfile.a

$(VITASDK)/arm-vita-eabi/lib/libvorbisfile.a:
	./scripts/setup-vitasdk.sh

3ds-toolchain: $(DEVKITPRO)/.complete-3ds

$(DEVKITPRO)/.complete-3ds:
	./scripts/setup-devkitpro.sh 3ds

wii-toolchain: $(DEVKITPRO)/.complete-wii

$(DEVKITPRO)/.complete-wii:
	./scripts/setup-devkitpro.sh wii

wiiu-toolchain: $(DEVKITPRO)/.complete-wiiu

$(DEVKITPRO)/.complete-wiiu:
	./scripts/setup-devkitpro.sh wiiu

ps2-toolchain: $(PS2DEV)/ee/bin/mips64r5900el-ps2-elf-gcc

$(PS2DEV)/ee/bin/mips64r5900el-ps2-elf-gcc:
	./scripts/setup-ps2dev.sh

ps3-toolchain: $(PS3DEV)/portlibs/ppu/lib/libvorbisfile.a

$(PS3DEV)/portlibs/ppu/lib/libvorbisfile.a:
	./scripts/setup-ps3dev.sh

xbox360-toolchain: $(DEVKITXENON)/usr/lib/libfat.a

$(DEVKITXENON)/usr/lib/libfat.a:
	./scripts/setup-xenon.sh

ctrtools:
	./scripts/setup-ctrtools.sh

vita3k: $(VITA3K)

$(VITA3K):
	./scripts/setup-vita3k.sh

zig: $(ZIG)

$(ZIG):
	./scripts/setup-zig.sh

engines: $(CONSOLES)

psp: psp-toolchain
	cmake -S engine -B build/psp -DCMAKE_TOOLCHAIN_FILE=$(PSPDEV)/psp/share/pspdev.cmake -DCMAKE_BUILD_TYPE=Release
	cmake --build build/psp -j$(JOBS)

vita: vita-toolchain
	cmake -S engine -B build/vita -DCMAKE_TOOLCHAIN_FILE=$(VITASDK)/share/vita.toolchain.cmake -DCMAKE_BUILD_TYPE=Release
	cmake --build build/vita -j$(JOBS)

3ds: 3ds-toolchain
	cmake -S engine -B build/3ds -DCMAKE_TOOLCHAIN_FILE=$(DEVKITPRO)/cmake/3DS.cmake -DCMAKE_BUILD_TYPE=Release -DCTRTOOLS="$$(./scripts/setup-ctrtools.sh)"
	cmake --build build/3ds -j$(JOBS)

wii: wii-toolchain
	cmake -S engine -B build/wii -DCMAKE_TOOLCHAIN_FILE=$(DEVKITPRO)/cmake/Wii.cmake -DCMAKE_BUILD_TYPE=Release
	cmake --build build/wii -j$(JOBS)

wiiu: wiiu-toolchain
	cmake -S engine -B build/wiiu -DCMAKE_TOOLCHAIN_FILE=$(DEVKITPRO)/cmake/WiiU.cmake -DCMAKE_BUILD_TYPE=Release
	cmake --build build/wiiu -j$(JOBS)

ps2: ps2-toolchain
	cmake -S engine -B build/ps2 -DCMAKE_TOOLCHAIN_FILE=$(PS2SDK)/ps2dev.cmake -DCMAKE_BUILD_TYPE=Release
	cmake --build build/ps2 -j$(JOBS)

ps3: ps3-toolchain
	cmake -S engine -B build/ps3 -DCMAKE_TOOLCHAIN_FILE=$(CURDIR)/engine/cmake/ps3.toolchain.cmake -DCMAKE_BUILD_TYPE=Release
	cmake --build build/ps3 -j$(JOBS)

xbox360: xbox360-toolchain
	cmake -S engine -B build/xenon -DCMAKE_TOOLCHAIN_FILE=$(CURDIR)/engine/cmake/xenon.toolchain.cmake -DCMAKE_BUILD_TYPE=Release
	cmake --build build/xenon -j$(JOBS)

vita-shaders: vita-toolchain vita3k
	./scripts/build-vita-shaders.sh

wiiu-shaders:
	./scripts/build-wiiu-shaders.sh

xbox360-shaders:
	python3 scripts/build-xenon-shaders.py

desktop:
	cmake -S engine -B build/desktop-$(TARGET) -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTCOAAL_TARGET=$(or $(PROFILE_$(TARGET)),$(TARGET))
	cmake --build build/desktop-$(TARGET) -j$(JOBS)

builder: engines
	cmake -S tools/builder -B build/builder-native -DCMAKE_BUILD_TYPE=Release
	cmake --build build/builder-native -j$(JOBS)

test: builder
	ctest --test-dir build/builder-native --output-on-failure

test-psp1000:
	./scripts/test-psp1000.sh $(EBOOT)

run: builder
	$(BUILDER) $(if $(GAME),"$(GAME)" -o "$(OUT)" --target $(TARGET) $(RUN_$(TARGET))) $(if $(VOICES),--voices "$(VOICES)")

play: desktop
	build/desktop-$(TARGET)/tcoaal --data "$(OUT)/$(PLAY_$(TARGET))" --saves "$(OUT)/saves"

release: engines zig
	ZIG=$(ZIG) ./scripts/build-builder.sh

web: engines zig
	ZIG=$(ZIG) ./scripts/build-web.sh

web-serve: web
	cd build/web && python3 -m http.server 8080 --bind 127.0.0.1

clean:
	rm -rf build
