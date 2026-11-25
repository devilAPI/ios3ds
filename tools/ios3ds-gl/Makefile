# ios3ds-gl: OpenGL ES 1.1 engine for iOS 4.2.1 (replaces MBXGLEngine) and
# its test program. See docs/gpu.md.
#   make            -> build/rootfs-add/.../OpenGLES.framework/MBXGLEngine.bundle/MBXGLEngine
#                      build/rootfs-add/usr/libexec/ios3ds-gltest
#   make host-test  -> the same engine core + gltest built for the build host,
#                      run offscreen on the software rasterizer and on the PICA200
#                      back end (against the software GPU model) and compared;
#                      writes build/gl-host/gltest.ppm and gltest.ppm.pica.ppm
#   make slots CACHE=<dyld_shared_cache_armv6>   regenerate gl_slots.h
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
CC   := $(ROOT)/scripts/ios-cc
ADD  := $(ROOT)/build/rootfs-add
ENGINE := $(ADD)/System/Library/Frameworks/OpenGLES.framework/MBXGLEngine.bundle/MBXGLEngine
GLTEST := $(ADD)/usr/libexec/ios3ds-gltest
# iOS 3.x build of the same engine (-DGL_IOS3: 7E18's 822-slot dispatch
# table); installed only by rootfs/overlay-3.1.3.json
ENGINE3 := $(ADD)/ios3/MBXGLEngine
HOSTDIR := $(ROOT)/build/gl-host

CORE  := api.c draw.c raster.c pica_be.c ca.c stereo.c pvrtc.c
HDRS  := engine.h gl_defs.h gl_slots.h pvrtc.h
# boot/common/stereo.h: the shared depth classes and layer classifier (docs/stereo3d.md)
STEREO := $(ROOT)/boot/common/stereo.h
# PICA200 back end (docs/pica200.md): command encoders shared with the tests;
# the GPU itself is the kernel shim on iOS and the software model on the host
PICA  := ../ios3ds-pica
PICA_SRC := $(PICA)/src/pica_cmd.c $(PICA)/src/pica_state.c $(PICA)/src/pica_tex.c $(PICA)/src/pica_ff.c
PICA_IOS := $(PICA_SRC) $(PICA)/src/pica_dev_ios.c
PICA_HOST := $(PICA_SRC) $(PICA)/src/pica_dev_host.c $(PICA)/src/pica_drv.c $(PICA)/sim/picasim.c
PICA_HDRS := $(wildcard $(PICA)/include/*.h $(PICA)/src/*.h $(PICA)/sim/*.h)
CFLAGS := -O2 -Wall -Wextra -Wno-unused-parameter -mfpu=vfp -DGL_PICA -I$(PICA)/include -I$(ROOT)/boot/common

all: $(ENGINE) $(ENGINE3) $(GLTEST)

$(ENGINE): $(CORE) iface.c thunks.S $(HDRS) $(STEREO) $(PICA_IOS) $(PICA_HDRS)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -bundle -fvisibility=hidden $(CORE) $(PICA_IOS) iface.c thunks.S \
	    -framework CoreFoundation -o $@

$(ENGINE3): $(CORE) iface.c thunks.S $(HDRS) $(STEREO) $(PICA_IOS) $(PICA_HDRS)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DGL_IOS3 -bundle -fvisibility=hidden $(CORE) $(PICA_IOS) iface.c thunks.S \
	    -framework CoreFoundation -o $@

$(GLTEST): test/gltest.c test/glapi.h gl_defs.h
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) test/gltest.c -framework OpenGLES -lobjc -o $@

host-test: $(HOSTDIR)/pvrtc-test $(HOSTDIR)/gltest
	$(HOSTDIR)/pvrtc-test
	$(HOSTDIR)/gltest $(HOSTDIR)/gltest.ppm
	@echo "--- again, every target written back by the GPU itself (the IOSurface present path)"
	IOS3DS_GL_PRESENT_ALL=1 $(HOSTDIR)/gltest
	@echo "--- again, every destination refused as not contiguous: the arena copy must pass the present check"
	IOS3DS_GL_PRESENT_ALL=1 IOS3DS_HOST_PRESENT=0 $(HOSTDIR)/gltest > $(HOSTDIR)/bounce.log 2>&1; \
	    st=$$?; cat $(HOSTDIR)/bounce.log; test $$st = 0 && grep -q "through an arena copy" $(HOSTDIR)/bounce.log && \
	    grep -q "present check passed" $(HOSTDIR)/bounce.log && ! grep -q "from now on" $(HOSTDIR)/bounce.log
	@echo "--- again, forced by the shim's PRESENT test flag (IOS3DS_GL_SCATTER=1)"
	IOS3DS_GL_SCATTER=1 IOS3DS_GL_PRESENT_ALL=1 $(HOSTDIR)/gltest > $(HOSTDIR)/flag.log 2>&1; \
	    st=$$?; cat $(HOSTDIR)/flag.log; test $$st = 0 && grep -q "through an arena copy" $(HOSTDIR)/flag.log && \
	    grep -q "present check passed" $(HOSTDIR)/flag.log && ! grep -q "from now on" $(HOSTDIR)/flag.log
	@echo "--- again, with presents that swap R and B (a wrong swizzle on hardware): the check must catch it"
	IOS3DS_GL_PRESENT_ALL=1 IOS3DS_HOST_PRESENT=swap $(HOSTDIR)/gltest > $(HOSTDIR)/swap.log 2>&1; \
	    st=$$?; cat $(HOSTDIR)/swap.log; test $$st = 0 && grep -q "present check FAILED" $(HOSTDIR)/swap.log

$(HOSTDIR)/pvrtc-test: pvrtc.c pvrtc.h test/test_pvrtc.c
	mkdir -p $(dir $@)
	cc -O2 -g -Wall -Wextra pvrtc.c test/test_pvrtc.c -o $@

$(HOSTDIR)/gltest: $(CORE) test/host_gl.c test/gltest.c test/glapi.h $(HDRS) $(STEREO) $(PICA_HOST) $(PICA_HDRS)
	mkdir -p $(dir $@)
	cc -DGL_HOST -DGL_PICA -I$(PICA)/include -I$(ROOT)/boot/common -O2 -g -Wall -Wno-unused-result -Wno-array-bounds -Wno-maybe-uninitialized \
	    $(CORE) $(PICA_HOST) test/host_gl.c test/gltest.c -lm -o $@

slots:
	python3 genslots.py $(CACHE) -o gl_slots.h

clean:
	rm -f $(ENGINE) $(ENGINE3) $(GLTEST)
	rm -rf $(HOSTDIR)
.PHONY: all host-test slots clean
