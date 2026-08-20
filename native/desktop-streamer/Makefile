CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS += $(shell pkg-config --cflags libdrm 2>/dev/null)
LDLIBS_DRM += $(shell pkg-config --libs libdrm 2>/dev/null)
LDLIBS_RGA += $(shell pkg-config --libs librga 2>/dev/null)
LDLIBS_MPP += $(shell pkg-config --libs rockchip_mpp 2>/dev/null)

ifneq ($(strip $(RK3399_RGA_ROOT)),)
CPPFLAGS += -I$(RK3399_RGA_ROOT)/include -DREMYDESK_RGA_LEGACY_API=1
LDLIBS_RGA := $(RK3399_RGA_ROOT)/librga.so.2
endif

all: drm_fb_probe drm_rga_snapshot drm_rga_mpp_frame drm_rga_mpp_stream

drm_fb_probe: drm_fb_probe.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDLIBS_DRM)

drm_rga_snapshot: drm_rga_snapshot.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDLIBS_DRM) $(LDLIBS_RGA)

drm_rga_mpp_frame: drm_rga_mpp_frame.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDLIBS_DRM) $(LDLIBS_RGA) $(LDLIBS_MPP)

drm_rga_mpp_stream: drm_rga_mpp_stream.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDLIBS_DRM) $(LDLIBS_RGA) $(LDLIBS_MPP)

clean:
	rm -f drm_fb_probe drm_rga_snapshot drm_rga_mpp_frame drm_rga_mpp_stream
