# Production WebKit Patch Series

This directory contains the browser's upstream WebKit integration changes.
The clean upstream/device baseline is recorded in `revision`; `series` lists
the patches in application order. `inputs` lists support sources copied into
the isolated worktree after patch replay; their content is included in the
runtime manifest's patch-series hash. `../webkit-overlays.json` maps the
project-owned DRM and Rockchip source files to their build destinations.
Those files are copied after replay and must never also appear in a patch.

## Patch Groups

- `0100-wpe-drm-platform.patch`: build integration and raw-touch seat ownership.
- `0200-skia-raster-compositor.patch`: direct Skia CPU raster-to-SHM path,
  compositor scheduling and GL-fence avoidance for CPU devices.
- `0250-full-repaint-on-motion.patch`: forces complete target painting and
  full damage publication during async scrolling. Animation repaint is an
  explicit diagnostic opt-in because games animate continuously.
- `0260-imagebitmap-complete-decode.patch`: forces lazy first-frame decoding
  before rejecting Blob-backed `createImageBitmap()` resources.
- `0270-webaudio-missing-elements.patch`: fails WebAudio decoding cleanly when
  a required bundled GStreamer element is absent.
- `0300-webrtc-media-overlay.patch`: WebRTC negotiation, DataChannel,
  Rockchip MPP decode, direct audio and hole-punch video integration.
- `0310-system-volume-control.patch`: routes audio through the system default
  ALSA PCM and adds true mute for the direct WebRTC audio branch when the
  system Master control reaches its minimum.
- `0400-network-cloud-compat.patch`: scoped cloud TLS compatibility and
  WebSocket diagnostics required by the current cloud-game flow.
- `0500-adaptive-memory-pressure.patch`: configurable Linux system-memory
  thresholds with recovery hysteresis and the native cache control API.
- `0640-portrait-menu-and-gpu-frame-stability.patch`: GPU swapchain content
  validity and full repaint on invalid targets.
- `0650-gpu-fence-and-scanout-coherence.patch`: producer completion when
  no GL fence can be created, rather than publishing after only a flush.
- `0660-native-drag-and-skia-tile-readiness.patch`: keeps acknowledged browser
  drags in the UI process, cancels their DOM stream once, and ignores late
  replies from the claimed gesture. Skia tiles wait for worker completion
  before reading textures/fences and invalidate cached image views after updates.
  The launcher freezes content coordinates and uses unique contact sequences
  so toolbar resize and slot reuse cannot change an ongoing drag.
- `0670-active-page-memory-relief-cadence.patch`: prevents stable active-page
  caches from being discarded every two seconds while system pressure is normal.
  Growth of 32MB, policy escalation or system pressure bypasses the ten-second
  cooldown. The process kill check still runs on every measurement.

The native platform implementation is maintained in `wpe-drm/` as overlays;
Skia, input and media integration remain patches against the recorded revision.
Conversion from the former dual sources reproduced all 46 modified/new build
files byte for byte against `dev@150b4b5` before functional changes.

## Replaying

```sh
PROJECT_ROOT=$PWD \
WEBKIT_BASE=/path/to/clean/WebKit \
WEBKIT_WORKTREE=/path/to/worktrees/webkit-production \
scripts/prepare_webkit_worktree.sh
```

The helper refuses to overwrite a non-worktree directory. It creates a
detached worktree at the recorded revision, checks every patch, then applies
the series. `scripts/build_webrtc_runtime_pve.sh` invokes it automatically.

## Updating

1. Edit project overlays directly, or validate upstream changes in an isolated worktree.
2. Regenerate only the affected upstream subsystem patch against `revision`.
3. Run `npm test` and replay the full series from a fresh worktree.
4. Rebuild the runtime. The generated `build-manifest.json` must record the
   revision, patch-series hash, build flags and ELF SHA256 values.

Do not duplicate overlay implementations inside patches or patch a long-lived
shared WebKit tree in place.
