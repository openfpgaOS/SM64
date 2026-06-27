# Super Mario 64 — openfpgaOS custom core
#
# A standalone openFPGA core wrapping the SM64 PC-port engine, rendering
# through the os30 hardware vertex-triangle GPU (of_gpu.h).  Builds a
# single app.elf that the openfpgaOS ELF loader runs on the shared os30
# bitstream.
#
# Targets:
#   make           Build the core (app.elf + SD tree under build/pocket/sm64/)
#   make copy      Copy this core to the Pocket SD card
#   make package   Package this core into a ZIP
#   make test      Desktop SDL2 smoke build (app_pc)
#   make clean     Remove build artifacts

APP       = sm64
TARGET   ?= pocket
MISTER_IP ?= mister.local
SDK_DIR   = ../sdk
ROOT      = $(realpath $(CURDIR)/../..)
OBJ_DIR   = $(ROOT)/.obj/$(APP)
BUILD_DIR = $(OBJ_DIR)
DIST      = $(ROOT)/dist/$(APP)
OUT       = $(ROOT)/build/$(TARGET)/$(APP)
RUNTIME   = $(ROOT)/runtime

# ── SM64 source tree (moved under src/sm64/sm64) ─────────────────────
SM64   = sm64
SRC    = $(SM64)/src
BUILD  = $(SM64)/build/us_pocket

# Assets are compiled into the ELF (extracted via the upstream
# extract/generate steps under $(BUILD)); the ELF is self-contained.
ASSET_BLOBS ?=

# ── Sources ──────────────────────────────────────────────────────────
ENGINE_SRCS  = $(wildcard $(SRC)/engine/*.c)
GAME_SRCS    = $(filter-out $(SRC)/game/main.c, $(wildcard $(SRC)/game/*.c))
AUDIO_SRCS   = $(wildcard $(SRC)/audio/*.c)
MENU_SRCS    = $(wildcard $(SRC)/menu/*.c)
BUFFER_SRCS  = $(wildcard $(SRC)/buffers/*.c)
GODDARD_SRCS = $(wildcard $(SRC)/goddard/*.c) $(wildcard $(SRC)/goddard/dynlists/*.c)

# PC-port layer (selected) + the new openfpgaOS GPU renderer
PC_SRCS = $(SRC)/pc/pc_main.c \
          $(SRC)/pc/ultra_reimplementation.c \
          $(SRC)/pc/configfile.c \
          $(SRC)/pc/mixer.c \
          $(SRC)/pc/controller/controller_entry_point.c \
          $(SRC)/pc/gfx/gfx_pc.c \
          $(SRC)/pc/gfx/gfx_cc.c \
          $(SRC)/pc/gfx/gfx_gpu.c

# libultra reimplementation (non-N64 float subset)
ULTRA_SRCS = $(SM64)/lib/src/alBnkfNew.c \
             $(SM64)/lib/src/guLookAtRef.c \
             $(SM64)/lib/src/guMtxF2L.c \
             $(SM64)/lib/src/guNormalize.c \
             $(SM64)/lib/src/guOrthoF.c \
             $(SM64)/lib/src/guPerspectiveF.c \
             $(SM64)/lib/src/guRotateF.c \
             $(SM64)/lib/src/guScaleF.c \
             $(SM64)/lib/src/guTranslateF.c

LEVEL_SRCS = $(wildcard $(SM64)/levels/*.c) \
             $(wildcard $(SM64)/levels/*/leveldata.c) \
             $(wildcard $(SM64)/levels/*/script.c) \
             $(wildcard $(SM64)/levels/*/geo.c)
ACTOR_SRCS = $(wildcard $(SM64)/actors/*.c)
BIN_SRCS   = $(wildcard $(SM64)/bin/*.c) $(wildcard $(SM64)/bin/us/*.c)
DATA_SRCS  = $(wildcard $(SM64)/data/*.c)
ASSET_SRCS = $(wildcard $(SM64)/assets/*.c)
# Sound banks (sequences + sample banks).  sound_data.c #includes the four
# generated .inc.c blobs (sound_data.ctl/.tbl, sequences.bin, bank_sets) out
# of $(BUILD)/sound/ — see the "Sound data generation" rules below.
SOUND_SRCS = $(SM64)/sound/sound_data.c

GENERATED_SRCS = $(BUILD)/assets/mario_anim_data.c \
                 $(BUILD)/assets/demo_data.c \
                 $(wildcard $(BUILD)/bin/*_skybox.c)

# openfpgaOS platform backends (replace the raw-MMIO pocket/ layer)
OF_SRCS = pocket/main_of.c \
          pocket/wm_pocket.c \
          pocket/controller_pocket.c \
          pocket/audio_pocket.c \
          pocket/fx32_mtx.c

SRCS = $(ENGINE_SRCS) $(GAME_SRCS) $(AUDIO_SRCS) $(MENU_SRCS) $(BUFFER_SRCS) \
       $(GODDARD_SRCS) $(PC_SRCS) $(ULTRA_SRCS) $(LEVEL_SRCS) $(ACTOR_SRCS) \
       $(BIN_SRCS) $(DATA_SRCS) $(ASSET_SRCS) $(GENERATED_SRCS) $(SOUND_SRCS) \
       $(OF_SRCS)
SRCS_CXX =

# ── Compiler flags ───────────────────────────────────────────────────
# Include search: SM64 engine headers + generated build dir + app root
# (so "pocket/..." resolves).  musl/of_* headers come from sdk.mk.
CFLAGS  = -I. -I$(SM64)/include -I$(SRC) -I$(SRC)/pc -I$(SM64) \
          -I$(BUILD) -I$(BUILD)/include
CFLAGS += -std=gnu99 -fcommon -fno-strict-aliasing -fwrapv
# Performance: the engine is CPU-bound (display-list interp + transform +
# emit), so optimize aggressively.  rv32imafc has hardware single-float but
# no double, so keep all FP single (-fsingle-precision-constant) and let
# -ffast-math + -fno-math-errno inline lrintf/sqrtf to fcvt/fsqrt.s.
# Override the SDK's -falign-loops=64 (it padded ~9% of .text with NOPs on
# loops that run once per call) back to 8 — last wins.
# NOTE: -flto is intentionally OFF — the upstream SM64 decomp has benign
# cross-TU display-list type mismatches (-Wlto-type-mismatch) and the
# original Makefile disabled LTO; enable it only after a hardware soak.
CFLAGS += -O3 -ffast-math -fno-math-errno -fno-trapping-math
CFLAGS += -fsingle-precision-constant
CFLAGS += -falign-loops=8
CFLAGS += -DVERSION_US -DNON_MATCHING -DAVOID_UB -D_LANGUAGE_C \
          -DF3DEX_GBI_2E -DNO_SEGMENTED_MEMORY -DWIDESCREEN=0 \
          -DENABLE_SOFTRAST -DTARGET_OPENFPGA
# SM64 PC-port is warning-noisy; keep the build readable.
CFLAGS += -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function \
          -Wno-missing-field-initializers -Wno-sign-compare \
          -Wno-implicit-function-declaration -Wno-pointer-sign \
          -Wno-parentheses -Wno-unused-but-set-variable \
          -Wno-maybe-uninitialized -Wno-format
# Upstream SM64 decomp noise that only surfaces at -O3 (benign): missing
# returns on unreachable switch defaults, intentional uninit reads, ROM-DMA
# memcpy range false-positives.
CFLAGS += -Wno-return-type -Wno-uninitialized -Wno-implicit-fallthrough \
          -Wno-stringop-overread

.DEFAULT_GOAL := all

include $(SDK_DIR)/sdk.mk

# ── Sound data generation ────────────────────────────────────────────
# Mirror the upstream SM64 audio pipeline to produce the four sound blobs
# that sound_data.c #includes.  The RISC-V app target is little-endian and
# 32-bit, so the N64 sound structures must be assembled with matching word
# size (--endian little --bitwidth 32) — a 64-bit layout makes the ALSeqData
# pointer fields the wrong size and the engine reads garbage offsets.
SOUND_DIR        := $(SM64)/sound
SOUND_BIN_DIR    := $(BUILD)/sound
SOUND_ENDIAN     := --endian little --bitwidth 32
SOUND_BANK_FILES := $(wildcard $(SOUND_DIR)/sound_banks/*.json)
SOUND_SAMPLE_AIFFS := $(wildcard $(SOUND_DIR)/samples/*/*.aiff)
SOUND_SAMPLE_AIFCS := $(patsubst $(SOUND_DIR)/%.aiff,$(BUILD)/sound/%.aifc,$(SOUND_SAMPLE_AIFFS))
# Sequence files: prebuilt us/*.m64 plus 00_sound_player.s assembled to .m64.
SOUND_SEQ_M64    := $(wildcard $(SOUND_DIR)/sequences/us/*.m64) \
                    $(patsubst $(SOUND_DIR)/sequences/%.s,$(BUILD)/sound/sequences/%.m64,\
                        $(wildcard $(SOUND_DIR)/sequences/*.s))
SOUND_INC_FILES  := $(SOUND_BIN_DIR)/sound_data.ctl.inc.c \
                    $(SOUND_BIN_DIR)/sound_data.tbl.inc.c \
                    $(SOUND_BIN_DIR)/sequences.bin.inc.c \
                    $(SOUND_BIN_DIR)/bank_sets.inc.c

PYTHON   ?= python3
AIFF_EXTRACT_CODEBOOK := $(SM64)/tools/aiff_extract_codebook
VADPCM_ENC            := $(SM64)/tools/vadpcm_enc
ASSEMBLE_SOUND        := $(SM64)/tools/assemble_sound.py
# Sequence .s files are pure data directives (the seq_* macros emit only
# .byte/.half/.word), so the host assembler produces byte-identical .m64
# output to any cross target — use it to stay independent of the RISC-V
# toolchain prefix for this step.
HOST_AS      ?= as
HOST_OBJCOPY ?= objcopy

# .aiff -> .table -> .aifc (ADPCM-encoded sample)
$(BUILD)/sound/%.table: $(SOUND_DIR)/%.aiff
	@mkdir -p $(dir $@)
	$(AIFF_EXTRACT_CODEBOOK) $< > $@

$(BUILD)/sound/%.aifc: $(BUILD)/sound/%.table $(SOUND_DIR)/%.aiff
	$(VADPCM_ENC) -c $^ $@

# 00_sound_player.s (sequence bytecode, host-assembler safe: data only) -> .m64
$(BUILD)/sound/sequences/%.m64: $(SOUND_DIR)/sequences/%.s
	@mkdir -p $(dir $@)
	$(HOST_AS) -I $(SM64)/include -I $(BUILD) --defsym VERSION_US=1 -o $@.o $<
	$(HOST_OBJCOPY) -j .rodata $@.o -O binary $@

# Sample banks -> sound_data.ctl / .tbl
$(SOUND_BIN_DIR)/sound_data.ctl: $(SOUND_BANK_FILES) $(SOUND_SAMPLE_AIFCS) $(ASSEMBLE_SOUND)
	@mkdir -p $(dir $@)
	$(PYTHON) $(ASSEMBLE_SOUND) $(BUILD)/sound/samples/ $(SOUND_DIR)/sound_banks/ \
		$(SOUND_BIN_DIR)/sound_data.ctl $(SOUND_BIN_DIR)/sound_data.tbl \
		-DVERSION_US $(SOUND_ENDIAN)
$(SOUND_BIN_DIR)/sound_data.tbl: $(SOUND_BIN_DIR)/sound_data.ctl ;

# Sequences + bank_sets
$(SOUND_BIN_DIR)/sequences.bin: $(SOUND_BANK_FILES) $(SOUND_DIR)/sequences.json $(SOUND_SEQ_M64) $(ASSEMBLE_SOUND)
	@mkdir -p $(dir $@)
	$(PYTHON) $(ASSEMBLE_SOUND) --sequences $@ $(SOUND_BIN_DIR)/bank_sets \
		$(SOUND_DIR)/sound_banks/ $(SOUND_DIR)/sequences.json $(SOUND_SEQ_M64) \
		-DVERSION_US $(SOUND_ENDIAN)
$(SOUND_BIN_DIR)/bank_sets: $(SOUND_BIN_DIR)/sequences.bin ;

# Binary blob -> C byte array
$(SOUND_BIN_DIR)/%.inc.c: $(SOUND_BIN_DIR)/%
	hexdump -v -e '1/1 "0x%X,"' $< > $@
	echo >> $@

# sound_data.c pulls the blobs in via #include; force the right build order.
$(OBJ_DIR)/$(SM64)/sound/sound_data.o: $(SOUND_INC_FILES)

.PHONY: sound
sound: $(SOUND_INC_FILES)

# Build ELF, then assemble the SD-card image under build/<target>/<app>/
all: $(OBJ_DIR)/app.elf release
	@$(SIZE) $<

release: $(OBJ_DIR)/app.elf
	@$(SDK_DIR)/platforms/$(TARGET)/image.sh "$(APP)" "$(OBJ_DIR)/app.elf" "$(ROOT)" "$(DIST)" $(ASSET_BLOBS)

debug: all
	@$(ROOT)/scripts/debug.sh $$(ls -d $(OUT)/Assets/*/common 2>/dev/null)/$(APP).elf

copy: all
	@MISTER_IP="$(MISTER_IP)" $(SDK_DIR)/platforms/$(TARGET)/copy.sh "$(APP)" "$(OBJ_DIR)/app.elf"

package: all
	@$(ROOT)/scripts/package.sh $(APP)

test: app_pc

clean: sdk-clean
	rm -rf $(ROOT)/build/pocket/$(APP) $(ROOT)/build/mister/$(APP)

# Full wipe: also remove the SM64 decomp's generated tree (assets, level
# .inc.c, sound banks). `clean` is app-only and reuses these (they're slow to
# regenerate); use distclean to force a true from-scratch rebuild.
distclean: clean
	rm -rf $(SM64)/build

.PHONY: all release debug copy package test clean distclean
