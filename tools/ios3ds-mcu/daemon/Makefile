# ios3ds-mcud: 3DS MCU mailbox -> iOS (volume, brightness, accelerometer; docs/mcu.md)
#   make            -> build/rootfs-add/usr/libexec/ios3ds-mcud
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
CC   := $(ROOT)/scripts/ios-cc
OUT  ?= $(ROOT)/build/rootfs-add/usr/libexec/ios3ds-mcud
OUT3 ?= $(ROOT)/build/rootfs-add/ios3/ios3ds-mcud
OUT2 ?= $(ROOT)/build/rootfs-add/ios2/ios3ds-mcud
DEPS := mcud.c $(ROOT)/boot/common/mcu_proto.h $(ROOT)/boot/common/als_curve.h \
        $(ROOT)/boot/common/backlight.h

all: $(OUT) $(OUT3) $(OUT2)

$(OUT): mcud.c $(ROOT)/boot/common/mcu_proto.h $(ROOT)/boot/common/als_curve.h \
        $(ROOT)/boot/common/backlight.h
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra -I$(ROOT)/boot/common mcud.c -framework IOKit -framework CoreFoundation -lobjc -o $@

# iOS 3.x / 2.x builds (-DMCU_OLD: no IOHIDEventSystemClient); installed only
# by rootfs/overlay-3.1.3.json / overlay-2.2.1.json. The 2.x one is linked for
# 2.0 with classic lazy binding, our dyld_stub_binding_helper and r9 kept free
# (5H11a's dyld reads its thread pointer from r9), like ios3ds-input.
$(OUT3): $(DEPS)
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra -DMCU_OLD -I$(ROOT)/boot/common mcud.c -framework IOKit -framework CoreFoundation -lobjc -o $@

$(OUT2): $(DEPS) $(ROOT)/tools/common/dyld2_glue.S
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra -DMCU_OLD -target armv6-apple-ios2.0 -ffixed-r9 -Wl,-w -I$(ROOT)/boot/common mcud.c \
	    $(ROOT)/tools/common/dyld2_glue.S -framework IOKit -framework CoreFoundation -lobjc -o $@

clean:
	rm -f $(OUT) $(OUT3) $(OUT2)
.PHONY: all clean
