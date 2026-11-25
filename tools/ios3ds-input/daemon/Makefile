# ios3ds-input: 3DS pad/touch -> IOHID event injector (armv6 iOS 4.2 Mach-O).
# Standalone on purpose: nothing in the top-level build includes this yet.
#   make            -> build/rootfs-add/usr/libexec/ios3ds-input
# To ship it, also add com.ios3ds.input.plist to the rootfs recipe the way
# rootfs/files/com.ios3ds.sample.plist is (see docs/input-design.md).
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
CC   := $(ROOT)/scripts/ios-cc
OUT  ?= $(ROOT)/build/rootfs-add/usr/libexec/ios3ds-input
OUT3 ?= $(ROOT)/build/rootfs-add/ios3/ios3ds-input

all: $(OUT) $(OUT3)

$(OUT): input.c cpad.h n3ds.h $(ROOT)/boot/common/input_proto.h
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra -I$(ROOT)/boot/common input.c -framework IOKit -framework CoreFoundation -o $@

# iOS 3.x build (-DINPUT_IOS3, GSEvents only); installed only by
# rootfs/overlay-3.1.3.json
$(OUT3): input.c cpad.h n3ds.h $(ROOT)/boot/common/input_proto.h
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra -DINPUT_IOS3 -I$(ROOT)/boot/common input.c -framework IOKit -framework CoreFoundation -o $@

clean:
	rm -f $(OUT) $(OUT3)
.PHONY: all clean
