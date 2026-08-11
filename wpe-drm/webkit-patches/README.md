# Production WebKit Patch Series

This directory is the reproducible source of the browser's WebKit changes.
The clean upstream/device baseline is recorded in `revision`; `series` lists
the patches in application order.

## Patch Groups

- `0100-wpe-drm-platform.patch`: DRM scanout, native chrome, raw-touch seat
  ownership, video overlay and synchronized video-plane commits.
- `0150-drm-generation-order.patch`: orders page, chrome and page-flip
  generations so UI redraws cannot resurrect an uncommitted page frame.
- `0160-rga-inset-and-lazy-chrome-base.patch`: keeps toolbar-resized rotated
  frames on RGA and avoids full CPU base snapshots while chrome is idle.
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
- `0500-adaptive-memory-pressure.patch`: adds configurable Linux system-memory
  thresholds with recovery hysteresis, bounded native glyph caching and safe
  DRM chrome-cache trimming without releasing scanout or in-flight buffers.

The former experiment-by-experiment patches and full modified source mirrors
were consolidated after replaying the production series successfully against
`9effc8745be9b7b0fab96b29cc69716c9a5c4ae0`. Their history remains available
in Git before `pre-cleanup-20260731`.

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

1. Make and validate changes only in an isolated worktree.
2. Regenerate the affected subsystem patch against `revision`.
3. Run `npm test` and replay the full series from a fresh worktree.
4. Rebuild the runtime. The generated `build-manifest.json` must record the
   revision, patch-series hash, build flags and ELF SHA256 values.

Do not restore full `Source/` mirrors or patch a long-lived shared WebKit tree
in place.
