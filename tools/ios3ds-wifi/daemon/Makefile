# ios3ds-wifid: kern.ios3ds_net <-> IOUserEthernet bridge (armv6 iOS 4.2 Mach-O).
#   make  -> build/rootfs-add/usr/libexec/ios3ds-wifid + ios3ds-nettest (self-test client)
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
CC   := $(ROOT)/scripts/ios-cc
OUT  ?= $(ROOT)/build/rootfs-add/usr/libexec/ios3ds-wifid

.DEFAULT_GOAL := all
$(OUT): wifid.c ent.plist $(ROOT)/boot/common/wifi_proto.h
	mkdir -p $(dir $@)
	IOS_CC_ENTITLEMENTS=$(CURDIR)/ent.plist $(CC) -Os -Wall -Wextra -I$(ROOT)/boot/common wifid.c \
	    -framework IOKit -framework CoreFoundation -o $@

NETTEST ?= $(ROOT)/build/rootfs-add/usr/libexec/ios3ds-nettest
OUT2 ?= $(ROOT)/build/rootfs-add/ios2/ios3ds-wifid
NETTEST2 ?= $(ROOT)/build/rootfs-add/ios2/ios3ds-nettest
OLD2 := -target armv6-apple-ios2.0 -ffixed-r9 -Wl,-w
GLUE := $(ROOT)/tools/common/dyld2_glue.S
all: $(OUT) $(NETTEST) $(OUT2) $(NETTEST2)

# iOS 2.x: en0 is the shim's own ifnet (kern.ios3ds_eth, shim/kif_eth.c);
# installed only by rootfs/overlay-2.2.1.json
$(OUT2): wifid.c ../shim/kif_proto.h $(ROOT)/boot/common/wifi_proto.h $(GLUE)
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra -DWIFID_KIF $(OLD2) -I$(ROOT)/boot/common -I../shim wifid.c $(GLUE) \
	    -framework CoreFoundation -o $@

$(NETTEST2): nettest.c $(GLUE)
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra $(OLD2) nettest.c $(GLUE) -o $@

$(NETTEST): nettest.c
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra nettest.c -o $@

clean:
	rm -f $(OUT) $(NETTEST) $(OUT2) $(NETTEST2)
.PHONY: all clean
