# ios3ds top-level Makefile
#
#   make toolchain            check the cross compiler / python deps
#   make fwprep IPSW=path     decrypt kernel, device tree, rootfs into build/staging
#                             (IOS=4.1 -> build/fw-4.1/staging; default 4.2.1)
#   make firm                 3DS payload  -> build/boot/ios3ds.firm
#   make shim                 kernel shim  -> build/shim/{shim.bin,gpu.bin} (prebuilt for releases)
#   make launcher             Homebrew Launcher app -> build/launcher/ios3ds.3dsx (devkitARM + libctru)
#   make userland             our iOS binaries -> build/rootfs-add/ (scripts/build-ios-toolchain.sh)
#   make hfsplus              xpwn's HFS+ editor -> build/tools/hfsplus
#   make sd                   SD card tree -> build/sd/  (copy its contents to the card root;
#                             downloads Cydia's debs into build/cydia, CYDIA=0 to skip,
#                             TLSFIX=0 for no TLSFix)
#   make sdimg                same, plus a 2 GiB raw SD image for the emulator (build/sd.img)
#   make qemu                 build QEMU with the 'nintendo3ds' dev machine (build/qemu)
#   make emu                  boot build/sd.img in the emulator, print the SD log
#   make kscan                print kernel facts used by docs/research.md
#   make test                 offline unit tests (no Apple files needed)
#   make release VERSION=v1   release zip of our own builds -> build/release/ (docs/release.md)
#
# Nothing Apple-owned is ever written outside build/ (gitignored).

PYTHON  ?= python3
# CYDIA=0: build the rootfs without the downloaded Cydia/APT base system (docs/cydia.md)
ifneq ($(filter 0 no off,$(CYDIA)),)
MKSD_FLAGS += --no-cydia
endif
# TLSFIX=0: leave out TLSFix, the Substrate tweak that gives iOS TLS 1.2/1.3 (docs/certificates.md)
ifneq ($(filter 0 no off,$(TLSFIX)),)
MKSD_FLAGS += --no-tlsfix
endif
# SANDBOX=1: relax the sandbox so TLSFix reaches Safari/App Store apps (opt-in;
# Safari OOMs on the 128 MB device, docs/cydia.md)
ifneq ($(filter 1 yes on,$(SANDBOX)),)
MKSD_FLAGS += --sandbox-open
endif
# CARRY_FROM=/path/to/old/rootfs.hfs: carry user data from an explicit old image
# (developer path; install.py carries from the card by default)
ifneq ($(CARRY_FROM),)
MKSD_FLAGS += --carry-from $(CARRY_FROM)
endif
# IOS=VERSION selects a firmware from scripts/versions.py. The default keeps the
# old staging directory, patch set and recipe, so 4.2.1 builds stay unchanged.
BUILD   ?= build
IOS     ?= 4.2.1
ifeq ($(IOS),4.2.1)
STAGING := $(BUILD)/staging
else
STAGING := $(BUILD)/fw-$(IOS)/staging
endif
MKSD_FLAGS += --ios $(IOS) --staging $(STAGING)

USERLAND := tools/ios3ds-diag tools/ios3ds-sample tools/ios3ds-gl tools/ios3ds-input/daemon tools/ios3ds-wifi/daemon tools/ios3ds-wifi/a80211 tools/ios3ds-keychain tools/ios3ds-audio/tone tools/ios3ds-mcu/daemon tools/ios3ds-mcu/gyrotest tools/ios3ds-camera/vcd tools/ios3ds-gps

.PHONY: all help toolchain fwprep firm shim launcher userland music-itdb hfsplus sd sdimg qemu emu kscan test release clean

all: firm

help:
	@sed -n '3,19p' Makefile | sed 's/^# //'

toolchain:
	@scripts/setup-toolchain.sh

fwprep:
	@test -n "$(IPSW)" || { echo "usage: make fwprep IPSW=/path/to/iPod2,1_4.2.1_8C148_Restore.ipsw"; exit 2; }
	$(PYTHON) scripts/fwprep.py "$(IPSW)" -o $(STAGING) --ios $(IOS) $(FWPREP_FLAGS)

firm:
	$(MAKE) -C boot

shim:
	$(PYTHON) scripts/mkboot.py --shim-only $(BUILD)/shim

launcher:
	$(MAKE) -C tools/ios3ds-launcher

userland:
	for d in $(USERLAND); do $(MAKE) -C $$d || exit 1; done

# Host-only helper for the experimental iOS 2.2.1 Music library; needs libgpod.
music-itdb:
	$(MAKE) -C tools/music-itdb

hfsplus:
	scripts/build-hfsplus.sh

sd: firm
	$(PYTHON) scripts/mksd.py $(MKSD_FLAGS)

sdimg: firm
	$(PYTHON) scripts/mksd.py --image $(BUILD)/sd.img $(MKSD_FLAGS)

qemu:
	emu/build-qemu.sh

emu: sdimg
	$(PYTHON) emu/run.py --time $${T:-10} --shots $${SHOTS:-9.5}

kscan: $(STAGING)/kernelcache.macho
	$(PYTHON) scripts/kscan.py $<

$(STAGING)/kernelcache.macho:
	@echo "run 'make fwprep IPSW=...' first"; exit 2

test:
	$(PYTHON) -m unittest discover -s tests -v
	$(MAKE) -C tools/ios3ds-pica test
	$(MAKE) -C tools/ios3ds-gl host-test
	$(MAKE) -C tools/ios3ds-wifi test
	$(MAKE) -C tools/ios3ds-gps test
	$(CC) -Wall -Wextra -O1 -o $(BUILD)/test_cpad tools/ios3ds-input/tests/test_cpad.c && $(BUILD)/test_cpad
	$(CC) -Wall -Wextra -O1 -o $(BUILD)/test_n3ds tools/ios3ds-input/tests/test_n3ds.c && $(BUILD)/test_n3ds
	$(CC) -Wall -Wextra -O1 -Iboot/common -o $(BUILD)/test_als tests/test_als.c -lm && $(BUILD)/test_als
	$(CC) -Wall -Wextra -O1 -Iboot/common -Iboot/arm9 -o $(BUILD)/test_blkcache tests/test_blkcache.c boot/arm9/blkcache.c && $(BUILD)/test_blkcache
	$(PYTHON) scripts/check-stub-symbols.py

release:
	$(PYTHON) scripts/mkrelease.py $(if $(VERSION),--version $(VERSION))

clean:
	rm -rf $(BUILD)/boot $(BUILD)/sd $(BUILD)/sd.img $(BUILD)/emu
