# Host helper for the experimental iOS 2.2.1 Music library (docs/audio.md §10).
# Needs libgpod's headers and library (Debian/Ubuntu: apt install libgpod-dev).
# Only used when building an image for a version whose overlay sets
# "music_legacy_db"; default 4.2.1 builds never run it.
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
OUT  ?= $(ROOT)/build/mkitdb

CFLAGS ?= -O1 -Wall -Wextra
PKG_CONFIG ?= pkg-config
CFLAGS += $(shell $(PKG_CONFIG) --cflags glib-2.0 libgpod-1.0 2>/dev/null || $(PKG_CONFIG) --cflags glib-2.0)
LDLIBS += $(shell $(PKG_CONFIG) --libs glib-2.0 libgpod-1.0 2>/dev/null || echo -lgpod)

all: $(OUT)

$(OUT): mkitdb.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I/usr/include/gpod-1.0 $< -o $@ $(LDLIBS)

clean:
	rm -f $(OUT)
.PHONY: all clean
