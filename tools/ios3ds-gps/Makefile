ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
CC   := $(ROOT)/scripts/ios-cc
OUT  := $(ROOT)/build/rootfs-add/usr/bin/ios3ds-gps
OUT2 := $(ROOT)/build/rootfs-add/usr/libexec/ios3ds-gpstest
TEST := $(ROOT)/build/ios3ds-gps-test
# iOS 2.x builds (rootfs/overlay-2.2.1.json): 2.0 link, classic lazy binding, r9 free
OUTA := $(ROOT)/build/rootfs-add/ios2/ios3ds-gps
OUTB := $(ROOT)/build/rootfs-add/ios2/ios3ds-gpstest
GLUE := $(ROOT)/tools/common/dyld2_glue.S
OLD2 := -target armv6-apple-ios2.0 -ffixed-r9 -Wl,-w

all: $(OUT) $(OUT2) $(OUTA) $(OUTB)

$(OUTA): ios3ds-gps.c gps.c gps.h $(GLUE)
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra $(OLD2) ios3ds-gps.c gps.c $(GLUE) -framework CoreFoundation -o $@

$(OUTB): gpstest.c gps.h $(GLUE)
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra $(OLD2) gpstest.c $(GLUE) -framework CoreFoundation -o $@

$(OUT): ios3ds-gps.c gps.c gps.h
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra ios3ds-gps.c gps.c -framework CoreFoundation -o $@

$(OUT2): gpstest.c gps.h
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra gpstest.c -framework CoreFoundation -o $@

test: $(TEST)
	$(TEST)

$(TEST): gps.c tests/test_gps.c
	mkdir -p $(dir $@)
	cc -O1 -Wall -Wextra -Werror -I. gps.c tests/test_gps.c -lm -o $@

clean:
	rm -f $(OUT) $(OUT2) $(OUTA) $(OUTB) $(TEST)
.PHONY: all test clean