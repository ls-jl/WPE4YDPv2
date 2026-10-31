CC ?= cc
CXX ?= c++
CFLAGS ?= -O2 -Wall -Wextra

TOOLS := \
	drm_fb_dump \
	dmabuf_dump \
	evdev_touch_injector \
	rga_rotate_probe \
	udp_flow_meta \
	udp_pcap_capture \
	udp_pcap_summary \
	uinput_touch_injector \
	video_overlay_probe \
	webrtc_sdp_answer_probe

.PHONY: all clean

all: $(TOOLS)

%: %.c
	$(CC) $(CFLAGS) $< -o $@

rga_rotate_probe: rga_rotate_probe.c
	$(CC) $(CFLAGS) $< -o $@ -ldrm -ldl

video_overlay_probe: video_overlay_probe.cpp
	$(CXX) $(CFLAGS) -std=c++11 $< -o $@ -ldrm

clean:
	rm -f $(TOOLS)
