# ios3ds-diag: runs a test/diagnostic launchd job only with boot.cfg diag=1 (docs/boot-perf.md)
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
CC   := $(ROOT)/scripts/ios-cc
OUT  ?= $(ROOT)/build/rootfs-add/usr/libexec/ios3ds-diag
LOG  ?= $(ROOT)/build/rootfs-add/usr/lib/ios3ds-applog.dylib
OUT2 ?= $(ROOT)/build/rootfs-add/ios2/ios3ds-diag
LOG2 ?= $(ROOT)/build/rootfs-add/ios2/ios3ds-applog.dylib
GLUE := $(ROOT)/tools/common/dyld2_glue.S
OLD2 := -target armv6-apple-ios2.0 -ffixed-r9 -Wl,-w

all: $(OUT) $(LOG) $(OUT2) $(LOG2)

# ent.plist: SpringBoard wants com.apple.springboard.opensensitiveurl for -urlpoll
$(OUT): diag.c ent.plist
	mkdir -p $(dir $@)
	IOS_CC_ENTITLEMENTS=$(CURDIR)/ent.plist $(CC) -Os -Wall -Wextra diag.c -o $@

# app NSLog -> /dev/console with diag=1 (applog.c, re-log 62)
$(LOG): applog.c ../ios3ds-gps/gps.c ../ios3ds-gps/gps.h
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra -I../ios3ds-gps -dynamiclib -install_name /usr/lib/ios3ds-applog.dylib applog.c ../ios3ds-gps/gps.c -o $@

# iOS 2.x builds (installed only by rootfs/overlay-2.2.1.json): linked for 2.0
# with classic lazy binding, our dyld_stub_binding_helper and r9 kept free,
# like ios3ds-input/ios3ds-mcud. 3.x uses the 4.x builds above.
$(OUT2): diag.c $(GLUE)
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra $(OLD2) diag.c $(GLUE) -o $@

$(LOG2): applog.c ../ios3ds-gps/gps.c ../ios3ds-gps/gps.h $(GLUE)
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra $(OLD2) -DDYLD2_DYLIB -I../ios3ds-gps -dynamiclib -install_name /usr/lib/ios3ds-applog.dylib applog.c ../ios3ds-gps/gps.c $(GLUE) -o $@

clean:
	rm -f $(OUT) $(LOG) $(OUT2) $(LOG2)
.PHONY: all clean
