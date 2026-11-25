# ios3ds-tone: AudioQueue test tone (armv6 iOS 4.2 Mach-O), see tone.c.
#   make  -> build/rootfs-add/usr/libexec/ios3ds-tone
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
CC   := $(ROOT)/scripts/ios-cc
OUT  ?= $(ROOT)/build/rootfs-add/usr/libexec/ios3ds-tone
OUT3 ?= $(ROOT)/build/rootfs-add/ios3/ios3ds-tone
OUT2 ?= $(ROOT)/build/rootfs-add/ios2/ios3ds-tone

all: $(OUT) $(OUT3) $(OUT2)

$(OUT): tone.c
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra tone.c -o $@

# iOS 3.x / 2.x builds, installed only by rootfs/overlay-3.1.3.json /
# overlay-2.2.1.json. Same source (AudioToolbox through dlopen); the 2.x one
# is linked like ios3ds-mcud (2.0 target, classic lazy binding, r9 kept free).
$(OUT3): tone.c
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra -DTONE_OLD tone.c -o $@

$(OUT2): tone.c $(ROOT)/tools/common/dyld2_glue.S
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra -DTONE_OLD -target armv6-apple-ios2.0 -ffixed-r9 -Wl,-w tone.c \
	    $(ROOT)/tools/common/dyld2_glue.S -o $@

clean:
	rm -f $(OUT) $(OUT3) $(OUT2)
.PHONY: all clean
