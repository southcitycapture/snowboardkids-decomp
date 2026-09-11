# Native port build for Snowboard Kids (N64 decomp -> PowerPC Mac OS X).
#
#   port/build-ppc.sh -j8          cross-build in Docker (the normal way)
#   make -C port list              show the game sources the port compiles
#
# Prerequisites: the N64 matching build has run once (it produces
# build/snowboardkids.map, which we mine for asset ROM offsets, and the
# splat linker script, which tells us exactly which C files are game code).

TARGET ?= ppc-darwin
ROOT   := ..
BUILD  := build-$(TARGET)
GEN    := $(BUILD)/gen

# ---- toolchain -----------------------------------------------------------------
ifeq ($(TARGET),ppc-darwin)
  CROSS    := /usr/local/bin/powerpc-apple-darwin8-
  SYSROOT  := /usr/local/MacOSX10.4u.sdk
  TARGET_CFLAGS  := -isysroot $(SYSROOT) -mmacosx-version-min=10.4 -malign-natural -mone-byte-bool
  # Link the image at 0x10000000 so native data pointers can never be mistaken
  # for N64 physical/segmented addresses (< 0x10000000) inside display lists.
  TARGET_LDFLAGS := -isysroot $(SYSROOT) -mmacosx-version-min=10.4 -Wl,-pagezero_size,0x10000000 \
                    -framework OpenGL -framework Cocoa -framework Carbon -framework IOKit \
                    -framework CoreAudio -framework AudioUnit -framework AudioToolbox \
                    -framework CoreFoundation -framework ForceFeedback -liconv -static-libgcc \
                    -Wl,-undefined,dynamic_lookup  # IOHIDManager (10.5) symbols are not in the 10.4u SDK stubs
  SYM_PREFIX := _
  CTX_ASM    := src/ultra/sbk_ctx_ppc_darwin.s
  EXE        := $(BUILD)/snowboardkids
else
  $(error unknown TARGET $(TARGET))
endif

CC      := $(CROSS)gcc
AS      := $(CROSS)as
PYTHON  ?= python3
SDL2_PREFIX ?= /work/sdl2

# ---- game sources: exactly the objects the N64 linker script links --------------
LD_SCRIPT  := $(ROOT)/snowboardkids.ld
N64_OBJS   := $(shell grep -oE 'build/src/[^ ]+\.o' $(LD_SCRIPT) | sort -u)
N64_C      := $(patsubst build/%.o,%.c,$(N64_OBJS))
# Everything under src/ except libultra itself, the RSP blobs and the MIPS entrypoint...
# system_runtime_buffers.c holds the heap, RSP buffers and framebuffers: pure
# BSS that lives in the emulated RDRAM like every other pinned global.
GAME_C     := $(filter-out src/ultra/% src/rsp/% src/boot/% src/engine/system_runtime_buffers.c,$(N64_C))
# ...plus the pure-C parts of libultra we keep: the audio synthesizer and gu math.
ULTRA_C    := $(filter src/ultra/audio/%.c src/ultra/gu/%.c src/ultra/io/pfs%.c src/ultra/io/contpfs.c,$(N64_C))
ULTRA_C    := $(filter-out src/ultra/io/pfsisplug.c src/ultra/io/pfsgetstatus.c,$(ULTRA_C)) # those talk to the PIF; os_pfs.c replaces them
# Linker scripts whose assignments are address arithmetic (asset/segment addresses,
# aliases into pinned data).
ABS_LD := $(wildcard $(ROOT)/linker_scripts/*.ld) $(ROOT)/undefined_syms_auto.txt
ABS_LD := $(filter-out %/hardware_regs.ld,$(ABS_LD))
# libultra globals the port defines itself (os_misc.c) must not become absolutes.
PORT_DEFINED := osTvType osRomBase osResetType osAppNMIBuffer osMemSize
# The port's own platform layer.
PORT_C     := $(wildcard src/*.c src/ultra/*.c src/gfx/*.c src/audio/*.c src/platform/*.c src/debug/*.c src/ui/*.c)
PORT_ASM   := $(CTX_ASM)

GAME_OBJS  := $(patsubst %.c,$(BUILD)/game/%.o,$(GAME_C) $(ULTRA_C))
PORT_OBJS  := $(patsubst %.c,$(BUILD)/port/%.o,$(PORT_C)) $(patsubst %.s,$(BUILD)/port/%.o,$(PORT_ASM))
ROM_SYMS   := $(BUILD)/rom_syms.o

# ---- flags -----------------------------------------------------------------------
GAME_DEFS  := -DPORT -DLANGUAGE_C -D_LANGUAGE_C -D_MIPS_SZLONG=32 -DNDEBUG -DCOMPILING_LIBULTRA \
              -DBUILD_VERSION=VERSION_I -DF3DEX_GBI -DMIPSEB -D_MIPS_SZINT=32 -D_MIPS_SZPTR=32 \
              -D_MIPS_ISA_MIPS2=2 -D_MIPS_ISA=_MIPS_ISA_MIPS2 -D_ABIO32=1 -D_MIPS_SIM=_ABIO32 -D_MIPS_FPSET=16 -D__sgi=1 \
              '-D__builtin_classof(type)=0'
GAME_INC   := -I$(ROOT) -I$(ROOT)/include -I$(ROOT)/include/PR -I$(ROOT)/src/ultra/audio -I$(ROOT)/src/ultra/libc
GAME_CFLAGS := -std=gnu89 -O2 -fno-builtin -fno-strict-aliasing -fwrapv -fno-common -funsigned-char \
               -Wno-unknown-pragmas -Wno-multichar -Wno-pointer-sign -w \
               -include src/port_override.h $(GAME_DEFS) $(GAME_INC) $(TARGET_CFLAGS)
PORT_CFLAGS := -std=gnu99 -O2 -g -fno-strict-aliasing -Wall -Wno-unused-parameter \
               -DPORT -D_LANGUAGE_C -DF3DEX_GBI $(GAME_INC) -Isrc -I$(SDL2_PREFIX)/include \
               $(TARGET_CFLAGS)
LDFLAGS := $(TARGET_LDFLAGS) $(SDL2_PREFIX)/lib/libSDL2.a -lm

# Initialised data upstream still keeps as assembly and that the N64 link uses
# (translated to twins; their BSS siblings need nothing: RDRAM starts zeroed).
DATA_S := $(ROOT)/asm/data/DFF40.data.s $(ROOT)/asm/data/E1CA0.rodata.s \
          $(ROOT)/asm/data/E29A0.rodata.s $(ROOT)/asm/data/E2B00.rodata.s
DATA_OBJS := $(patsubst $(ROOT)/asm/data/%.s,$(BUILD)/data/%.o,$(DATA_S))

# ---- pinned globals (see tools/gen_pins.py) ------------------------------------
PINS_STAMP := $(BUILD)/pins.stamp
PINS_S     := $(BUILD)/pins.s
PINS_TXT   := $(BUILD)/pins.txt
PIN_TABLE  := $(BUILD)/pin_table.c
OBJ_LIST   := $(BUILD)/n64objs.txt
N64_NM     := $(BUILD)/n64syms.txt
PIN_OBJS   := $(BUILD)/pins.o $(BUILD)/pin_table.o

# ---- rules ------------------------------------------------------------------------
all: $(EXE)

.SECONDARY:

# The game's entry point is called main(); keep it out of the host's way.
$(BUILD)/game/src/engine/system_runtime.o: GAME_CFLAGS += -Dmain=sbk_game_main

# port_aliases.c wraps game functions, so it is compiled as game code.
$(BUILD)/port/src/port_aliases.o: PORT_CFLAGS = $(GAME_CFLAGS)

$(BUILD)/data/%.s: $(ROOT)/asm/data/%.s tools/asm2darwin.py
	@mkdir -p $(dir $@)
	$(PYTHON) tools/asm2darwin.py $< --prefix '$(SYM_PREFIX)' --suffix __sbk_unpinned --out $@

$(BUILD)/data/%.o: $(BUILD)/data/%.s
	$(AS) -o $@ $<

list:
	@echo "GAME_C ($(words $(GAME_C))):"; printf '  %s\n' $(GAME_C)
	@echo "ULTRA_C ($(words $(ULTRA_C))):"; printf '  %s\n' $(ULTRA_C)
	@echo "PORT_C ($(words $(PORT_C))):"; printf '  %s\n' $(PORT_C)

# Game sources are compiled from a filtered mirror: `#pragma weak A = B`
# aliases (IDO-only; they crash GCC on Mach-O) become harmless unknown pragmas
# and the port supplies the aliases as real functions in src/port_aliases.c.
$(OBJ_LIST): Makefile
	@mkdir -p $(dir $@)
	@printf '%s\n' $(patsubst %.c,build/%.o,$(GAME_C) $(ULTRA_C)) $(patsubst $(ROOT)/asm/data/%.s,build/asm/data/%.o,$(DATA_S)) > $@

$(PINS_STAMP): $(ROOT)/build/snowboardkids.map $(OBJ_LIST) tools/gen_pins.py
	$(PYTHON) tools/gen_pins.py $< --objs $(OBJ_LIST) $(if $(wildcard $(N64_NM)),--nm $(N64_NM)) --prefix '$(SYM_PREFIX)' --pins-s $(PINS_S) --pins-txt $(PINS_TXT) --table-c $(PIN_TABLE)
	@touch $@

$(PINS_S) $(PINS_TXT) $(PIN_TABLE): $(PINS_STAMP)

$(BUILD)/pins.o: $(PINS_S)
	$(AS) -o $@ $<

$(BUILD)/pin_table.o: $(PIN_TABLE) src/ultra/sbk_pins.h
	$(CC) $(PORT_CFLAGS) -c -o $@ $<

# The game's string literals use its own character encoding: upstream converts
# them with tools/textconv.py before compiling, and so must we.
$(GEN)/%.c: $(ROOT)/%.c tools/mirror_src.py patches.txt $(PINS_TXT) $(ROOT)/tools/textconv.py $(ROOT)/tools/charmap.txt
	@mkdir -p $(dir $@)
	@$(PYTHON) $(ROOT)/tools/textconv.py $(ROOT)/tools/charmap.txt $< $@.tc
	@$(PYTHON) tools/mirror_src.py --pins $(PINS_TXT) $@.tc $@
	@rm -f $@.tc

# Included fragments (.inc.c, libmus command files) are pulled in by the
# main file's #include; mirror them too so the include finds the filtered copy.
GEN_FRAGMENTS := $(patsubst $(ROOT)/%,$(GEN)/%,$(shell find $(ROOT)/src -name '*.inc.c' -not -path '*/ultra/*') $(shell find $(ROOT)/src/libmus -name 'player_*.c'))

$(BUILD)/game/%.o: $(GEN)/%.c $(GEN_FRAGMENTS) src/port_override.h
	@mkdir -p $(dir $@)
	$(CC) $(GAME_CFLAGS) -I$(dir $<) -I$(ROOT)/$(dir $*) -c -o $@ $<

# the Xbox One driver includes the 10.4 SDK USB headers through a patched copy (compat/)
$(BUILD)/port/src/platform/input_xone.o: src/platform/input_xone.c
	@mkdir -p $(dir $@)
	$(CC) $(PORT_CFLAGS) -Isrc/platform/compat -c -o $@ $<

$(BUILD)/port/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(PORT_CFLAGS) -c -o $@ $<

$(BUILD)/port/%.o: %.s
	@mkdir -p $(dir $@)
	$(AS) -o $@ $<

$(BUILD)/rom_syms.s: $(ROOT)/build/snowboardkids.map tools/gen_rom_syms.py $(PINS_TXT)
	@mkdir -p $(dir $@)
	$(PYTHON) tools/gen_rom_syms.py $< $(foreach l,$(ABS_LD),--ld $(l)) --extra-abs $(PINS_TXT) $(foreach s,$(PORT_DEFINED),--skip $(s)) --prefix '$(SYM_PREFIX)' --out $@

$(ROM_SYMS): $(BUILD)/rom_syms.s
	$(AS) -o $@ $<

$(EXE): $(GAME_OBJS) $(PORT_OBJS) $(DATA_OBJS) $(ROM_SYMS) $(PIN_OBJS) Makefile
	$(CC) -o $@ $(GAME_OBJS) $(PORT_OBJS) $(DATA_OBJS) $(ROM_SYMS) $(PIN_OBJS) $(LDFLAGS)
	@echo "built $@"

clean:
	rm -rf $(BUILD)

.PHONY: all list clean

game-objs: $(GAME_OBJS)
	@echo "game objects built"
.PHONY: game-objs
