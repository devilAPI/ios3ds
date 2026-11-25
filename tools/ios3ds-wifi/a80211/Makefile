# ios3ds-a80211.dylib: the Apple80211 driver surface for configd's WiFiManager
# (interposed ioctl/IOKit calls, backed by kern.ios3ds_wlan; docs/wifi.md 10).
#   make  -> build/rootfs-add/usr/lib/ios3ds-a80211.dylib
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
CC   := $(ROOT)/scripts/ios-cc
OUT  ?= $(ROOT)/build/rootfs-add/usr/lib/ios3ds-a80211.dylib
.DEFAULT_GOAL := all

$(OUT): a80211.c $(ROOT)/boot/common/wifi_proto.h
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra -Wno-unused-parameter -I$(ROOT)/boot/common -dynamiclib \
	    -install_name /usr/lib/ios3ds-a80211.dylib a80211.c \
	    -framework IOKit -framework CoreFoundation -framework SystemConfiguration -o $@

# iOS 2.x (5H11a): the same ioctl surface (Apple80211.framework is a dylib
# there but speaks the 3.x protocol); classic lazy binding for 2.x dyld.
# Installed only by rootfs/overlay-2.2.1.json
OUT2 ?= $(ROOT)/build/rootfs-add/ios2/ios3ds-a80211.dylib
GLUE := $(ROOT)/tools/common/dyld2_glue.S
all: $(OUT) $(OUT2)
$(OUT2): a80211.c $(ROOT)/boot/common/wifi_proto.h $(GLUE)
	mkdir -p $(dir $@)
	$(CC) -Os -Wall -Wextra -Wno-unused-parameter -DA80211_IOS2 -DDYLD2_DYLIB \
	    -target armv6-apple-ios2.0 -ffixed-r9 -Wl,-w -I$(ROOT)/boot/common -dynamiclib \
	    -install_name /usr/lib/ios3ds-a80211.dylib a80211.c $(GLUE) \
	    -framework IOKit -framework CoreFoundation -framework SystemConfiguration -o $@

clean:
	rm -f $(OUT) $(OUT2)
.PHONY: all clean
