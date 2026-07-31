CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

TOOLS := \
	drm_fb_dump \
	evdev_touch_injector \
	udp_flow_meta \
	udp_pcap_capture \
	udp_pcap_summary \
	uinput_touch_injector \
	webrtc_sdp_answer_probe

.PHONY: all clean

all: $(TOOLS)

%: %.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f $(TOOLS)
