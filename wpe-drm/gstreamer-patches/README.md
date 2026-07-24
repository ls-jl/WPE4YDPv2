# GStreamer Runtime Patches

`0001-webrtcbin-update-remote-offer-transceivers.patch` updates GStreamer
1.22 `webrtcbin` as soon as a remote Offer is accepted. Without it,
transceivers remain invisible to WebKit until after a local Answer is set.
Cloud-game clients that wait for `track` before calling `createAnswer()` then
deadlock and eventually report a first-frame timeout such as `-5001`.
The patch also preserves the remote offer codec and payload mapping on those
early transceivers so GStreamer 1.22 reuses them when creating the Answer.

The patch is applied by `scripts/build_webrtc_runtime_pve.sh` before building
the bundled WebRTC GStreamer plugins. The same script synchronizes the
repository's `GStreamerMediaEndpoint.cpp` mirror before rebuilding WebKit;
that WebKit-side compatibility layer applies cloud-game BUNDLE and ICE-role
workarounds only to the configured cloud-game domains.
