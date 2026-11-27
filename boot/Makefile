# ios3ds 3DS payload (M1): builds build/boot/ios3ds.firm
# Works with devkitARM or any arm-none-eabi-gcc (CROSS=... to override).
# SPDX-License-Identifier: GPL-3.0-or-later

ROOT    := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
OUT     ?= $(ROOT)/build/boot
CROSS   ?= $(shell $(ROOT)/scripts/setup-toolchain.sh 2>/dev/null | sed -n 's/^\[ok\]   cross compiler: \(.*\)gcc .*/\1/p')
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
PYTHON  ?= python3

GITREV  := $(shell git -C $(ROOT) describe --always --dirty --tags 2>/dev/null || echo nogit)
VERSION ?= m1-$(GITREV)-$(shell date -u +%Y%m%d.%H%M)

COMMON_CFLAGS := -MMD -MP -Os -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -ffreestanding \
                 -fno-builtin -fno-tree-loop-distribute-patterns -fno-stack-protector \
                 -ffunction-sections -fdata-sections -marm \
                 -I$(ROOT)/boot/common -I$(ROOT)/boot/arm9 -DIOS3DS_IDLE=1 -DIOS3DS_VERSION=\"$(VERSION)\"
ARM9_FLAGS  := -mcpu=arm946e-s -mfloat-abi=soft
ARM11_FLAGS := -mcpu=mpcore -mfloat-abi=soft
LDFLAGS     := -nostdlib -Wl,--gc-sections -Wl,--no-warn-rwx-segments

ARM9_SRC  := arm9/start.S arm9/main.c arm9/bootcfg.c arm9/console.c arm9/log.c arm9/sdmmc.c arm9/diskio.c arm9/blksrv.c arm9/blkcache.c arm9/hfsfix.c arm9/display.c arm9/tsc.c arm9/swap.c arm9/audio.c arm9/gpumirror.c arm9/i2c.c arm9/mcu.c arm9/gyro.c arm9/exthid.c arm9/camera.c arm9/powersave.c arm9/power.c \
             $(addprefix arm9/wifi/,wifi.c wifi_cfg.c wifi_ring.c wifi_frame.c wpa2.c sdio.c ar6k.c vnic.c nwmcode.c) \
             arm9/fatfs/ff.c arm9/fatfs/ffunicode.c common/util.c
ARM11_SRC := arm11/start.S arm11/main.c common/util.c

ARM9_OBJ  := $(patsubst %,$(OUT)/arm9/%.o,$(ARM9_SRC))
ARM11_OBJ := $(patsubst %,$(OUT)/arm11/%.o,$(ARM11_SRC))
LIBGCC9   = $(shell $(CC) $(ARM9_FLAGS) -marm -print-libgcc-file-name)
LIBGCC11  = $(shell $(CC) $(ARM11_FLAGS) -marm -print-libgcc-file-name)

# VERSION is compiled into both stubs, but git describe/date change on every
# make invocation: without this stamp an incremental build can leave one stub
# showing the previous commit (hardware run3: loader g64b0cd8, ARM11 g89178de).
# The stamp is only rewritten when the version actually changes, so it does
# not force a full rebuild on every make.
VERSION_STAMP := $(OUT)/.version
.PHONY: FORCE_VERSION
FORCE_VERSION:
$(VERSION_STAMP): FORCE_VERSION
	@mkdir -p $(dir $@)
	@echo "$(VERSION)" | cmp -s - $@ 2>/dev/null || echo "$(VERSION)" > $@
$(OUT)/arm9/arm9/main.c.o $(OUT)/arm11/arm11/main.c.o: $(VERSION_STAMP)

.PHONY: all clean
# the version-stamp rule below appears before this target; keep `make -C boot`
# building the firmware, not just the phony stamp
.DEFAULT_GOAL := all
all: $(OUT)/ios3ds.firm

$(OUT)/arm9/%.o: $(ROOT)/boot/%
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) $(ARM9_FLAGS) -c $< -o $@

$(OUT)/arm11/%.o: $(ROOT)/boot/%
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) $(ARM11_FLAGS) -c $< -o $@

$(OUT)/arm9.elf: $(ARM9_OBJ) arm9/link.ld
	$(CC) $(ARM9_FLAGS) $(LDFLAGS) -T arm9/link.ld $(ARM9_OBJ) $(LIBGCC9) -o $@

$(OUT)/arm11.elf: $(ARM11_OBJ) arm11/link.ld
	$(CC) $(ARM11_FLAGS) $(LDFLAGS) -T arm11/link.ld $(ARM11_OBJ) $(LIBGCC11) -o $@

$(OUT)/%.bin: $(OUT)/%.elf
	$(OBJCOPY) -O binary $< $@

$(OUT)/ios3ds.firm: $(OUT)/arm9.bin $(OUT)/arm11.bin $(ROOT)/scripts/mkfirm.py
	$(PYTHON) $(ROOT)/scripts/mkfirm.py $@ --screen-init \
	    --arm9-entry 0x08000040 --arm11-entry 0x1FF90000 \
	    --section 0x08000040:$(OUT)/arm9.bin --section 0x1FF90000:$(OUT)/arm11.bin
	@echo "$(VERSION)" > $(OUT)/VERSION
	@echo "payload version: $(VERSION)"

clean:
	rm -rf $(OUT)

-include $(ARM9_OBJ:.o=.d) $(ARM11_OBJ:.o=.d)
