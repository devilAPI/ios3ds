# ios3ds-input: 3DS pad/touch -> IOHID event injector (armv6 iOS 4.2 Mach-O).
# Standalone on purpose: nothing in the top-level build includes this yet.
#   make            -> build/rootfs-add/usr/libexec/ios3ds-input
# To ship it, also add com.ios3ds.input.plist to the rootfs recipe the way
# rootfs/files/com.ios3ds.sample.plist is (see docs/input-design.md).
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
CC   := $(ROOT)/scripts/ios-cc
OUT  ?= $(ROOT)/build/rootfs-add/usr/libexec/ios3ds-input
OUT3 ?= $(ROOT)/build/rootfs-add/ios3/ios3ds-input
OUT2 ?= $(ROOT)/build/rootfs-add/ios2/ios3ds-input
OUT41 ?= $(ROOT)/build/rootfs-add/ios41/ios3ds-input

all: $(OUT) $(OUT3) $(OUT2) $(OUT41)

$(OUT): input.c cpad.h n3ds.h $(ROOT)/boot/common/input_proto.h
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra -I$(ROOT)/boot/common input.c -framework IOKit -framework CoreFoundation -o $@

# iOS 3.x build (-DINPUT_IOS3, GSEvents only); installed only by
# rootfs/overlay-3.1.3.json. INPUT_PURPLE_FALLBACK fixes in-app taps on 7E18.
$(OUT3): input.c cpad.h n3ds.h $(ROOT)/boot/common/input_proto.h
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra -DINPUT_IOS3 -DINPUT_PURPLE_FALLBACK -I$(ROOT)/boot/common input.c -framework IOKit -framework CoreFoundation -o $@

# iOS 4.1 has the same purple-port suffix mismatch as 3.1.3; the 4.2.1 binary
# above is compiled without this flag and stays byte-identical.
$(OUT41): input.c cpad.h n3ds.h $(ROOT)/boot/common/input_proto.h
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra -DINPUT_PURPLE_FALLBACK -I$(ROOT)/boot/common input.c -framework IOKit -framework CoreFoundation -o $@

# iOS 2.x build: the 3.x source, linked for 2.0 (classic lazy binding; 5H11a's
# dyld has no LC_DYLD_INFO / dyld_stub_binder) with our dyld_stub_binding_helper;
# installed only by rootfs/overlay-2.2.1.json
$(OUT2): input.c cpad.h n3ds.h $(ROOT)/boot/common/input_proto.h $(ROOT)/tools/common/dyld2_glue.S
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra -DINPUT_IOS3 -DINPUT_IOS2 -DINPUT_PURPLE_FALLBACK -target armv6-apple-ios2.0 -ffixed-r9 -Wl,-w -I$(ROOT)/boot/common input.c \
	    $(ROOT)/tools/common/dyld2_glue.S -framework IOKit -framework CoreFoundation -o $@

clean:
	rm -f $(OUT) $(OUT3) $(OUT2) $(OUT41)
.PHONY: all clean
