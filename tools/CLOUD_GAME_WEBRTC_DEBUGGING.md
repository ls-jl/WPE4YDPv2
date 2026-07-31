# Cloud Game WebRTC Runbook

This document records the production state and the shortest safe diagnostic
path. Historical experiments remain in Git before `pre-cleanup-20260731`.

## Current Pipeline

```text
WebRTC H.264 RTP
  -> GStreamer webrtcbin / rtpjitterbuffer
  -> h264parse (byte-stream, AU aligned)
  -> Rockchip mppvideodec
  -> NV12 DMA-BUF
  -> WebKit hole-punch socket
  -> WPE DRM video overlay plane
```

Remote Opus audio is decoded in the WebRTC endpoint and sent directly to the
platform audio sink. DataChannel remains in WebKit/GStreamer and carries game
control data. Camera, microphone and screen capture permissions are denied.

## Confirmed Requirements

- WebKit built with `ENABLE_WEB_RTC`, `USE_GSTREAMER_WEBRTC`,
  `ENABLE_MEDIA_STREAM`, `ENABLE_VIDEO`, `ENABLE_WEBGL`, `USE_GBM` and
  `USE_SKIA`; the separate GPU process remains disabled.
- Bundled GStreamer includes WebRTC, NICE, RTP/RTPManager, SRTP, DTLS, SCTP,
  Opus and the Rockchip MPP plugin.
- H.264 must reach MPP as byte-stream access units. Reintroducing arbitrary
  NAL alignment causes a short burst of decoded frames followed by a freeze.
- The video overlay owns one stable sink at a time and is acquired only after
  the first valid DMA-BUF frame.
- Video-plane commits are synchronous. The previous nonblocking commit plus
  guessed framebuffer retirement caused visible old/new-frame flashing.
- Incoming remote audio bypasses MediaStream player preroll to avoid blocking
  the shared BUNDLE transport.
- Cloud-game WebSocket TLS compatibility is scoped to expired certificates on
  `*.mhystatic.com:5443`. Unknown CA, hostname mismatch and other TLS errors
  remain rejected.

## Production Environment

Defaults are defined in `wpe-drm/run.sh` and the WebKit production patch
series. Important rollback switches:

```sh
WPE_WEBRTC=1
WPE_WEBRTC_CAPTURE=deny
WPE_VIDEO_OVERLAY=1
WPE_VIDEO_OVERLAY_FIT=contain
WEBKIT_GST_HOLE_PUNCH_QUIRK=rockchip
WEBKIT_GST_WEBRTC_FORCE_EARLY_VIDEO_DECODING=1
WEBKIT_GST_WEBRTC_DIRECT_REMOTE_AUDIO=1
WEBKIT_GST_WEBRTC_COALESCE_INCOMING_VIDEO=1
WEBKIT_GST_WEBRTC_REPARSE_H264=1
WEBKIT_GST_MPP_REQUIRE_H264_AU=1
```

Set `WPE_VIDEO_OVERLAY=0` only to isolate overlay failures. Set
`WPE_GPU_MODE=off` to isolate Mali/EGL issues while retaining the CPU browser
path. Diagnostic logging is opt-in with `WPE_CLOUD_DIAGNOSTICS=1`; it records
state and sizes, never SDP fingerprints, candidates, authentication values or
message payloads.

## Fast Validation

1. Start from the MiniApp launcher and enter the cloud game normally. Do not
   leave the game running after the check.
2. Confirm processes:

   ```sh
   pidof wpe-drm-minimal WPEWebProcess WPENetworkProcess
   ```

3. Inspect the app browser log and look for ICE/DataChannel/media progress:

   ```sh
   grep -E 'ICE|DataChannel|overlay|mpp|video|audio|Frame rendered' \
     /path/to/browser/wpe-drm.log | tail -n 200
   ```

4. Confirm the video plane and NV12 framebuffer:

   ```sh
   cat /sys/kernel/debug/dri/0/state
   ```

5. Exit immediately and verify cleanup:

   ```sh
   kill -TERM "$(pidof wpe-drm-minimal)" 2>/dev/null || true
   pidof wpe-drm-minimal WPEWebProcess WPENetworkProcess || true
   ```

## Failure Classification

### `-5001` Before Media

Check WSS/TLS, ICE completion and DataChannel state in that order. A loaded
HTML shell does not prove WebRTC signaling completed. Do not change DRM or MPP
until the connection path is confirmed.

### Audio Works, Video Missing

Check for H.264 parser output, MPP decoded frame growth, overlay socket owner
and an NV12 framebuffer in DRM state. If decoded frames advance but the plane
does not, isolate with `WPE_VIDEO_OVERLAY=0`. If decode stops after a few
frames, verify AU alignment and allocation negotiation.

### Video Flickers

Confirm the runtime contains the synchronous video-overlay commit patch and
that logs report no failed commits. Ensure only one `wpe-drm-minimal` owns the
overlay socket. Do not re-enable nonblocking commits or time-based framebuffer
retirement.

### Video Is Stretched or Rotated

`panelSize` is the browser coordinate space; `drmMode` is only the scanout
envelope. Verify `WPE_VIDEO_OVERLAY_ROTATION`, `WPE_VIDEO_OVERLAY_FIT=contain`
and the published visible video rectangle. Touch mapping must use that frozen
rectangle for the entire gesture.

### Touch Only Rotates Camera

Game input uses hybrid classification: short low-motion contacts become
trusted pointer taps; drags and multi-touch become native touch sequences.
Check gesture logs and the frozen video mapping before changing thresholds.

### Black or Partial Primary Frame

This is separate from WebRTC. CPU read paths must wait for the rendering fence
and perform full-frame copies by default. Keep `WPE_DRM_PARTIAL_COPY=0` while
diagnosing; a fence timeout must retain the previous complete frame.

## Build And Rollback

The authoritative WebKit baseline and patches are in
`wpe-drm/webkit-patches/{revision,series}`. Rebuild only through an isolated
worktree:

```sh
PROJECT_ROOT=$PWD JOBS=72 scripts/build_webrtc_runtime_pve.sh
```

Every staged runtime must contain `build-manifest.json`. Compare its WebKit
revision, patch-series hash and ELF SHA256 before deployment. The last known
pre-cleanup browser can be restored from Git tag `pre-cleanup-20260731`.

## Test Discipline

- Keep cloud-game validation to the shortest connection, video and touch test.
- Exit the cloud game and stop WPE immediately after validation.
- Record observable counters and exact runtime hashes; distinguish browser
  shell load, signaling, media decode and DRM presentation.
- Change one subsystem at a time. Preserve a CPU fallback and a previous AMR
  before deploying GPU, WebRTC or DRM changes.
