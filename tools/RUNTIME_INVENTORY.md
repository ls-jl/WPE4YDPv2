# Runtime Inventory

Snapshot: 2026-07-31. Generate fresh values with:

```sh
du -sh assets/wpe-runtime/{lib,assets/fonts,lib/gstreamer-1.0}
find assets/wpe-runtime/lib/gstreamer-1.0 -type f -name '*.so' | wc -l
```

## Current Size

| Area | Size | Notes |
| --- | ---: | --- |
| Complete runtime | 384 MiB | Installed inside the AMR; no external runtime fallback |
| Runtime libraries | 247 MiB | Includes WebKit, ICU, Mesa, RGA, TLS and media dependencies |
| Fonts | 88 MiB | 44 CJK/Latin font files |
| GStreamer plugins | 16 MiB | 126 plugins; WebRTC/video/audio feature set retained |

Largest individual files are `libWPEWebKit-2.0.so.1` (about 103 MiB stripped; upstream file version 1.10.2),
the proprietary Mali library (about 42 MiB), ICU data (about 30 MiB), and CJK
fonts. `swrast_dri.so` and `kms_swrast_dri.so` intentionally share an inode in
the packaged filesystem where hard links are preserved.

The WebKit runtime intentionally has no `.so` or `.so.1.10.2` aliases. Falcon
dereferences symlinks while creating an AMR, so aliases would become duplicate
archive entries and separate files after installation. `librga.so.2` is also
packaged as one regular file and is loaded only through its explicit package
path.

## Retention Rules

- Keep the complete bundled GStreamer set until site playback, WebRTC receive,
  DataChannel, Opus, MPP decode and DRM video overlay have automated coverage.
- WebAudio decoding requires `libgstinterleave.so` in addition to the usual
  decoder, audioconvert and audioresample plugins. Missing it presents as Ogg
  `not-linked`, failed `.opus` assets and repeated game resource retries.
- HTML video playback requires `libgstdeinterlace.so` when GStreamer inserts
  `deinterlace` for interlaced or unknown-interlace streams. Missing it can
  leave Poki previews or game media blank even when the decoder is present.
- AVIF support consists of bundled `libavif.so.16` plus `libdav1d.so.7`; both
  are runtime requirements when WebKit is built with `USE_AVIF=ON`.
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
