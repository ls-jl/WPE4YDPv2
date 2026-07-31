# Runtime Inventory

Snapshot: 2026-07-31. Generate fresh values with:

```sh
du -sh assets/wpe-runtime/{lib,assets/fonts,lib/gstreamer-1.0}
find assets/wpe-runtime/lib/gstreamer-1.0 -type f -name '*.so' | wc -l
```

## Current Size

| Area | Size | Notes |
| --- | ---: | --- |
| Complete runtime | 429 MiB | Installed inside the AMR; no external runtime fallback |
| Runtime libraries | 292 MiB | Includes WebKit, ICU, Mesa, TLS and media dependencies |
| Fonts | 88 MiB | 44 CJK/Latin font files |
| GStreamer plugins | 16 MiB | 126 plugins; WebRTC/video/audio feature set retained |

Largest individual files are `libWPEWebKit-2.0.so.1.10.2` (about 149 MiB),
the proprietary Mali library (about 42 MiB), ICU data (about 30 MiB), and CJK
fonts. `swrast_dri.so` and `kms_swrast_dri.so` intentionally share an inode in
the packaged filesystem where hard links are preserved.

## Retention Rules

- Keep the complete bundled GStreamer set until site playback, WebRTC receive,
  DataChannel, Opus, MPP decode and DRM video overlay have automated coverage.
- Keep HarmonyOS Sans SC Regular/Medium/Bold and Noto Sans SC. Other CJK fonts
  are candidates only after a glyph-coverage comparison against all supported
  locales.
- Keep both CPU Mesa and Mali paths because GPU probing can fail per device and
  must fall back to Skia CPU rendering.
- Never add a fallback to `/userdisk/wpe-drm2`, `/userdisk/mesa` or a chroot.

## Future Whitelist Work

Any runtime reduction must be a separate change with an A/B AMR. Produce a
plugin load trace for Baidu, Bilibili, Douyin, local WebRTC and cloud gaming;
derive the GStreamer whitelist from the union of those traces. For fonts,
generate a Unicode coverage report first. Compare startup, playback, WebRTC,
Chinese text and Profile UI before accepting size savings.
