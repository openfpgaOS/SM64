# PocketSM64 Firmware Makefile
# Builds bootloader (BRAM) + SM64 engine (SDRAM)
#
# Prerequisites:
#   1. Place baserom.us.z64 in sm64/ directory
#   2. Run 'make extract' once to extract ROM assets
#   3. Run 'make generate' once to generate C/H files
#   4. Run 'make' to cross-compile for rv32imafc

# Toolchain
CROSS = riscv64-elf-
CC = $(CROSS)gcc
AS = $(CROSS)gcc
LD = $(CROSS)gcc
OBJCOPY = $(CROSS)objcopy
OBJDUMP = $(CROSS)objdump
SIZE = $(CROSS)size

# Target
TARGET = firmware

# Directories
FPGA_CORE_DIR = ../fpga/core
LIBC_DIR = libc
SM64_DIR = sm64
SM64_SRC = $(SM64_DIR)/src
SM64_BUILD = $(SM64_DIR)/build/us_pocket
POCKET_DIR = pocket

# Architecture
ARCH = rv32imafc
ABI = ilp32f

# === Source files ===

# Bootloader (goes in BRAM)
BOOT_SRCS_C = main.c terminal.c dataslot.c misaligned.c

# libc (linked into PSRAM with app)
LIBC_SRCS = $(LIBC_DIR)/memory.c \
            $(LIBC_DIR)/string.c \
            $(LIBC_DIR)/ctype.c \
            $(LIBC_DIR)/stdlib.c \
            $(LIBC_DIR)/qsort.c \
            $(LIBC_DIR)/time.c \
            $(LIBC_DIR)/math.c \
            $(LIBC_DIR)/file.c

LIBC_ASM_SRCS = $(LIBC_DIR)/setjmp.S

# SM64 engine sources
SM64_ENGINE_SRCS = $(wildcard $(SM64_SRC)/engine/*.c)
SM64_GAME_SRCS = $(filter-out $(SM64_SRC)/game/main.c, $(wildcard $(SM64_SRC)/game/*.c))
SM64_AUDIO_SRCS = $(wildcard $(SM64_SRC)/audio/*.c)
SM64_MENU_SRCS = $(wildcard $(SM64_SRC)/menu/*.c)
SM64_BUFFER_SRCS = $(wildcard $(SM64_SRC)/buffers/*.c)

# Goddard (Mario face renderer)
SM64_GODDARD_SRCS = $(wildcard $(SM64_SRC)/goddard/*.c) \
                    $(wildcard $(SM64_SRC)/goddard/dynlists/*.c)

# PC port layer (selected files only)
SM64_PC_SRCS = $(SM64_SRC)/pc/pc_main.c \
               $(SM64_SRC)/pc/ultra_reimplementation.c \
               $(SM64_SRC)/pc/configfile.c \
               $(SM64_SRC)/pc/mixer.c \
               $(SM64_SRC)/pc/controller/controller_entry_point.c \
               $(SM64_SRC)/pc/gfx/gfx_pc.c \
               $(SM64_SRC)/pc/gfx/gfx_cc.c \
               $(SM64_SRC)/pc/gfx/gfx_soft.c

# libultra reimplementation (non-N64 subset)
SM64_ULTRA_SRCS = $(SM64_DIR)/lib/src/alBnkfNew.c \
                  $(SM64_DIR)/lib/src/guLookAtRef.c \
                  $(SM64_DIR)/lib/src/guMtxF2L.c \
                  $(SM64_DIR)/lib/src/guNormalize.c \
                  $(SM64_DIR)/lib/src/guOrthoF.c \
                  $(SM64_DIR)/lib/src/guPerspectiveF.c \
                  $(SM64_DIR)/lib/src/guRotateF.c \
                  $(SM64_DIR)/lib/src/guScaleF.c \
                  $(SM64_DIR)/lib/src/guTranslateF.c

# Level, actor, and data sources (with assets compiled in)
SM64_LEVEL_SRCS = $(wildcard $(SM64_DIR)/levels/*.c) \
                  $(wildcard $(SM64_DIR)/levels/*/leveldata.c) \
                  $(wildcard $(SM64_DIR)/levels/*/script.c) \
                  $(wildcard $(SM64_DIR)/levels/*/geo.c)
SM64_ACTOR_SRCS = $(wildcard $(SM64_DIR)/actors/*.c)
SM64_BIN_SRCS = $(wildcard $(SM64_DIR)/bin/*.c) \
                $(wildcard $(SM64_DIR)/bin/us/*.c)
SM64_DATA_SRCS = $(wildcard $(SM64_DIR)/data/*.c)
SM64_ASSET_SRCS = $(wildcard $(SM64_DIR)/assets/*.c)

# Generated sources (created by 'make generate')
SM64_GENERATED_SRCS = $(SM64_BUILD)/assets/mario_anim_data.c \
                      $(SM64_BUILD)/assets/demo_data.c \
                      $(wildcard $(SM64_BUILD)/bin/*_skybox.c)

# Pocket platform backends (Phase 4)
POCKET_SRCS = $(wildcard $(POCKET_DIR)/*.c)

# All SM64 sources
SM64_ALL_SRCS = $(SM64_ENGINE_SRCS) $(SM64_GAME_SRCS) $(SM64_AUDIO_SRCS) \
                $(SM64_MENU_SRCS) $(SM64_BUFFER_SRCS) $(SM64_GODDARD_SRCS) \
                $(SM64_PC_SRCS) $(SM64_ULTRA_SRCS) \
                $(SM64_LEVEL_SRCS) $(SM64_ACTOR_SRCS) $(SM64_BIN_SRCS) \
                $(SM64_DATA_SRCS) $(SM64_ASSET_SRCS)

# Assembly sources
SRCS_S = start.S

# All sources
ALL_C_SRCS = $(BOOT_SRCS_C) $(LIBC_SRCS) $(SM64_ALL_SRCS) $(SM64_GENERATED_SRCS) $(POCKET_SRCS)
OBJS = $(patsubst %.c,$(SM64_BUILD)/pocket/%.o,$(ALL_C_SRCS)) \
       $(patsubst %.S,$(SM64_BUILD)/pocket/%.o,$(SRCS_S) $(LIBC_ASM_SRCS))

# === Compiler flags ===

# Common flags
COMMON_CFLAGS = -march=$(ARCH) -mabi=$(ABI) -Os -Wall -Wextra
COMMON_CFLAGS += -ffreestanding -nostdlib -nostartfiles
COMMON_CFLAGS += -ffunction-sections -fdata-sections
COMMON_CFLAGS += -fno-builtin
COMMON_CFLAGS += -fsingle-precision-constant

# SM64 include paths
# Our libc/ must come BEFORE sm64/include/libc so our wrapper headers
# (stdio.h, stdlib.h, etc.) are found first when SM64 code does #include <stdio.h>
SM64_INCLUDES = -I$(LIBC_DIR) \
                -I$(SM64_DIR)/include -I$(SM64_DIR)/include/libc \
                -I$(SM64_DIR)/src -I$(SM64_DIR)/src/pc -I$(SM64_DIR) \
                -I$(SM64_BUILD) -I$(SM64_BUILD)/include \
                -I. -I$(POCKET_DIR)

# SM64 defines
SM64_DEFINES = -DVERSION_US -DNON_MATCHING -DAVOID_UB \
               -D_LANGUAGE_C -DF3DEX_GBI_2E \
               -DENABLE_SOFTRAST -DTARGET_POCKET \
               -DNO_SEGMENTED_MEMORY -DWIDESCREEN=0

# Boot compiler flags (size-optimized)
BOOT_CFLAGS = $(COMMON_CFLAGS) -I. -I$(LIBC_DIR)

# SM64 compiler flags (speed-optimized)
SM64_CFLAGS = $(filter-out -Os,$(COMMON_CFLAGS)) -O2 -flto
SM64_CFLAGS += -std=gnu99 -fcommon $(SM64_INCLUDES) $(SM64_DEFINES)
SM64_CFLAGS += -Wno-unused-parameter -Wno-unused-variable
SM64_CFLAGS += -Wno-missing-field-initializers -Wno-sign-compare
SM64_CFLAGS += -Wno-implicit-function-declaration -Wno-pointer-sign
SM64_CFLAGS += -Wno-parentheses -Wno-unused-but-set-variable
SM64_CFLAGS += -fno-strict-aliasing -fwrapv

# Assembler flags
ASFLAGS = -march=$(ARCH)_zicsr -mabi=$(ABI)

# Linker flags
LDFLAGS = -march=$(ARCH) -mabi=$(ABI)
LDFLAGS += -T linker.ld -nostdlib -nostartfiles
LDFLAGS += -Wl,--gc-sections
LDFLAGS += -Wl,-Map=$(TARGET).map
LDFLAGS += -flto

# libgcc
LIBGCC = $(shell $(CC) -march=$(ARCH) -mabi=$(ABI) -print-libgcc-file-name)
LIBS = $(LIBGCC)

# RAM size for MIF generation (64KB = 16384 words)
RAM_WORDS = 16384

# === Build rules ===

# Default target
all: $(TARGET).bin $(TARGET).mif $(TARGET).lst sm64.bin
	$(SIZE) $(TARGET).elf

# Create build directories
$(SM64_BUILD)/pocket:
	@mkdir -p $@

# Link everything
$(TARGET).elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

# Full binary
$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

# BRAM image for MIF
$(TARGET).mif: $(TARGET).elf
	$(OBJCOPY) -O binary \
		--only-section=.boot \
		--only-section=.boot_data \
		--only-section=.boot_bss \
		--only-section=.fasttext \
		--only-section=.fastdata \
		$< boot.bin
	@echo "-- Firmware RAM initialization - $(RAM_WORDS) x 32-bit words" > $@
	@echo "-- Auto-generated from boot sections" >> $@
	@echo "" >> $@
	@echo "WIDTH=32;" >> $@
	@echo "DEPTH=$(RAM_WORDS);" >> $@
	@echo "" >> $@
	@echo "ADDRESS_RADIX=DEC;" >> $@
	@echo "DATA_RADIX=HEX;" >> $@
	@echo "" >> $@
	@echo "CONTENT BEGIN" >> $@
	@hexdump -v -e '1/4 "%08X\n"' boot.bin | awk '{printf "%d : %s;\n", NR-1, $$1}' >> $@
	@LAST=$$(hexdump -v -e '1/4 "%08X\n"' boot.bin | wc -l); \
	if [ $$LAST -lt $(RAM_WORDS) ]; then \
		echo "[$$LAST..$$(($(RAM_WORDS)-1))] : 00000013;" >> $@; \
	fi
	@echo "END;" >> $@
	@echo "Generated $@ with $$(hexdump -v -e '1/4 "%08X\n"' boot.bin | wc -l) words of BRAM image"

# Extract app binary (loaded by APF bridge to SDRAM, copied to PSRAM by bootloader)
sm64.bin: $(TARGET).elf
	$(OBJCOPY) -O binary --only-section=.text --only-section=.data $< $@
	@echo "Generated $@ ($$(wc -c < $@) bytes)"

# Disassembly listing
$(TARGET).lst: $(TARGET).elf
	$(OBJDUMP) -d -S $< > $@

# === Compilation rules ===

# Boot sources (size-optimized, go in BRAM)
$(SM64_BUILD)/pocket/%.o: %.c | $(SM64_BUILD)/pocket
	@mkdir -p $(dir $@)
	$(CC) $(BOOT_CFLAGS) -c -o $@ $<

# libc sources
$(SM64_BUILD)/pocket/$(LIBC_DIR)/%.o: $(LIBC_DIR)/%.c | $(SM64_BUILD)/pocket
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) -I. -I$(LIBC_DIR) -c -o $@ $<

# libc assembly
$(SM64_BUILD)/pocket/$(LIBC_DIR)/%.o: $(LIBC_DIR)/%.S | $(SM64_BUILD)/pocket
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -c -o $@ $<

# SM64 sources (speed-optimized)
$(SM64_BUILD)/pocket/$(SM64_DIR)/%.o: $(SM64_DIR)/%.c | $(SM64_BUILD)/pocket
	@mkdir -p $(dir $@)
	$(CC) $(SM64_CFLAGS) -c -o $@ $<

# SM64 generated sources
$(SM64_BUILD)/pocket/$(SM64_BUILD)/%.o: $(SM64_BUILD)/%.c | $(SM64_BUILD)/pocket
	@mkdir -p $(dir $@)
	$(CC) $(SM64_CFLAGS) -c -o $@ $<

# Pocket backend sources
$(SM64_BUILD)/pocket/$(POCKET_DIR)/%.o: $(POCKET_DIR)/%.c | $(SM64_BUILD)/pocket
	@mkdir -p $(dir $@)
	$(CC) $(SM64_CFLAGS) -c -o $@ $<

# Assembly sources
$(SM64_BUILD)/pocket/%.o: %.S | $(SM64_BUILD)/pocket
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -c -o $@ $<

# === Asset extraction and generation ===

# Step 1: Extract assets from ROM (run once, requires baserom.us.z64)
extract:
	@echo "Extracting SM64 assets from ROM..."
	@if [ ! -f "$(SM64_DIR)/baserom.us.z64" ]; then \
		echo "Error: Place baserom.us.z64 in $(SM64_DIR)/ directory"; \
		exit 1; \
	fi
	cd $(SM64_DIR) && python3 extract_assets.py us

# Step 2: Generate C/H files (text strings, animations, skyboxes, sound)
# Uses the SM64 Makefile with host compiler to generate intermediate files
generate: extract
	@echo "Generating SM64 build files..."
	@mkdir -p $(SM64_BUILD)/include
	@mkdir -p $(SM64_BUILD)/assets
	@mkdir -p $(SM64_BUILD)/bin
	@mkdir -p $(SM64_BUILD)/sound
	@echo "Generating level_headers.h..."
	cd $(SM64_DIR) && cc -E -P -x c -I . levels/level_headers.h.in | \
		python3 tools/output_level_headers.py > build/us_pocket/include/level_headers.h
	cd $(SM64_DIR) && $(MAKE) TARGET_N64=0 TARGET_DOS=0 ENABLE_SOFTRAST=1 \
		NOEXTRACT=1 VERSION=us BUILD_DIR=build/us_pocket \
		build/us_pocket/include/text_strings.h \
		build/us_pocket/include/text_menu_strings.h \
		build/us_pocket/assets/mario_anim_data.c \
		build/us_pocket/assets/demo_data.c
	@echo "Generating texture .inc.c files..."
	@cd $(SM64_DIR) && find . -name '*.rgba16.png' -o -name '*.ia8.png' \
		-o -name '*.ia16.png' -o -name '*.ia4.png' -o -name '*.ia1.png' \
		-o -name '*.i4.png' -o -name '*.i8.png' | while read png; do \
		rel=$${png#./}; \
		fmt=$$(echo "$$rel" | sed 's/.*\.\([^.]*\)\.png/\1/'); \
		bin="build/us_pocket/$${rel%.png}"; \
		inc="$${bin}.inc.c"; \
		if [ ! -f "$$inc" ] || [ "$$png" -nt "$$inc" ]; then \
			mkdir -p "$$(dirname "$$bin")"; \
			tools/n64graphics -i "$$bin" -g "$$rel" -f "$$fmt" && \
			hexdump -v -e '1/1 "0x%X,"' "$$bin" > "$$inc" && \
			echo >> "$$inc"; \
		fi; \
	done
	@echo "Generating text data..."
	@mkdir -p $(SM64_BUILD)/text/us
	cd $(SM64_DIR) && cpp -DVERSION_US text/define_text.inc.c -o - -I text/us/ | \
		tools/textconv charmap.txt - build/us_pocket/text/us/define_text.inc.c
	@echo "Generating skybox and cake textures..."
	@cd $(SM64_DIR) && for sky in textures/skyboxes/*.png; do \
		name=$$(basename "$$sky" .png); \
		if [ ! -f "build/us_pocket/bin/$${name}_skybox.c" ]; then \
			tools/skyconv --type sky --split "$$sky" build/us_pocket/bin; \
		fi; \
	done
	@cd $(SM64_DIR) && if [ ! -f "build/us_pocket/levels/ending/cake.inc.c" ]; then \
		mkdir -p build/us_pocket/levels/ending; \
		tools/skyconv --type cake --split levels/ending/cake.png build/us_pocket/levels/ending; \
	fi
	@echo "Texture generation complete."

# === Install and release ===

install: $(TARGET).mif
	cp $(TARGET).mif $(FPGA_CORE_DIR)/firmware.mif
	@echo "MIF installed to $(FPGA_CORE_DIR)/firmware.mif"

release: sm64.bin
	mkdir -p ../../release/Assets/pocketsm64/common
	cp sm64.bin ../../release/Assets/pocketsm64/common/
	@echo "sm64.bin copied to release/Assets/pocketsm64/common/"

# === Clean ===

clean:
	rm -rf $(SM64_BUILD)/pocket
	rm -f $(TARGET).elf $(TARGET).bin $(TARGET).mif $(TARGET).map $(TARGET).lst
	rm -f boot.bin sm64.bin

clean-all: clean
	rm -rf $(SM64_BUILD)

rebuild: clean all

# Show memory usage
mem: $(TARGET).elf
	$(SIZE) -A -x $(TARGET).elf

# Show all source files that will be compiled
sources:
	@echo "=== Boot sources ==="
	@echo $(BOOT_SRCS_C) | tr ' ' '\n'
	@echo "=== Libc sources ==="
	@echo $(LIBC_SRCS) | tr ' ' '\n'
	@echo "=== SM64 sources ==="
	@echo $(SM64_ALL_SRCS) | tr ' ' '\n'
	@echo "=== Pocket sources ==="
	@echo $(POCKET_SRCS) | tr ' ' '\n'
	@echo "=== Total C files: $$(echo $(ALL_C_SRCS) | tr ' ' '\n' | wc -l) ==="

.PHONY: all clean clean-all rebuild install release mem extract generate sources
