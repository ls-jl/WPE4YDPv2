# Third-Party Licenses

This project bundles and/or derives from source files belonging to the
[WPE WebKit](https://wpewebkit.org/) project (copyright Igalia S.L. and
contributors). Those files are **not** covered by the root `LICENSE` (MIT)
of this repository — they retain their original upstream licenses as
described below.

## 1. BSD-2-Clause files (`wpe-drm/` — WPEPlatform DRM backend mirror)

The following files are local mirrors of files from
`Source/WebKit/WPEPlatform/wpe/drm/` in the WPE WebKit tree and carry a
BSD-2-Clause header (`Copyright (C) 2023 Igalia S.L.`):

- `wpe-drm/WPEViewDRM.cpp`
- `wpe-drm/WPEDisplayDRM.cpp`
- `wpe-drm/WPEDisplayDRMPrivate.h`
- `wpe-drm/WPEDRM.h`
- `wpe-drm/WPEDRM.cpp`
- `wpe-drm/WPEToplevelDRM.cpp`
- (and any other files under `wpe-drm/` carrying the same header)

These files must retain their original copyright notice and disclaimer.
Redistribution in source or binary form is permitted under the following
terms:

```
Copyright (C) 2023 Igalia S.L.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## 2. LGPL-2.0-or-later files (`wpe-drm/` — WebCore GStreamer quirk)

The following file is derived from / follows the header convention of
WebCore source files (copyright Igalia S.L.) and is licensed under the
**GNU Library General Public License (LGPL), version 2 or later**:

- `wpe-drm/GStreamerHolePunchQuirkRockchip.cpp`
- `wpe-drm/GStreamerHolePunchQuirkRockchip.h`

```
Copyright (C) 2026 Igalia S.L

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Library General Public
License as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
Library General Public License for more details.

You should have received a copy of the GNU Library General Public License
along with this library; see the file COPYING.LIB. If not, write to
the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
Boston, MA 02110-1301, USA.
```

The full text of the GNU Library General Public License, version 2, can be
found at: https://www.gnu.org/licenses/old-licenses/lgpl-2.0.html

### ⚠️ Compliance note — LGPL "corresponding source" obligation

Because this file (and other WebKit-side patches described in
`wpe-drm/webkit-patches/README.md`) is compiled into the custom
WebKit 2.53.3 runtime (upstream file version 1.10.2), shipped as the single
`libWPEWebKit-2.0.so.1` binary inside
`assets/wpe-runtime/lib/` as part of the packaged `.amr`, the LGPL requires
that recipients of the binary be able to obtain (or be given a way to
rebuild/relink) the corresponding source of the LGPL-covered portions.

To stay compliant, this project should:

1. Keep this repository's `wpe-drm/` mirror and `webkit-patches/` diffs
   up to date with what is actually compiled into the shipped runtime
   (per the existing `scripts/sync_generated.sh` process).
2. Publish or otherwise make available the full patched WebKit source tree
   (or a diff against the corresponding upstream WPE WebKit release tag)
   that was used to build the packaged `libWPEWebKit-2.0.so.1`, so that a recipient
   of the `.amr` package can reproduce or relink the library.
3. Retain all original copyright/license headers in any file copied from,
   or patched against, the WebKit source tree.

If the full patched WebKit source tree cannot be published for any reason,
consult legal counsel — simply shipping the compiled `.so` without a way to
obtain corresponding source does not satisfy the LGPL.

## 3. Rockchip librga (Apache-2.0)

`assets/wpe-runtime/lib/librga.so.2` is the Rockchip RGA userspace
library used for hardware-assisted framebuffer rotation. The bundled ARM64
binary has SHA-256
`8935a536e27ec6a88b258a7886ab656076317d5aedfa9edd9aa0019a752ebc3c`.
The minimal IM2D ABI declarations in `wpe-drm/WPEViewDRM.cpp` and
`tools/diagnostics/rga_rotate_probe.c` mirror the upstream Apache-2.0 headers
from https://github.com/airockchip/librga.

Rockchip librga is licensed under the Apache License, Version 2.0. A copy of
that license is available at https://www.apache.org/licenses/LICENSE-2.0.

## 4. Other WebKit subsystem patches (not vendored in this repo)

`wpe-drm/webkit-patches/README.md` documents additional source-level patches
applied directly to the build-server WebKit tree (outside files mirrored
here), touching:

- `Source/WebKit/WebProcess/WebPage/CoordinatedGraphics/AcceleratedSurface.cpp`
- `Source/WebCore/platform/gstreamer/GStreamerQuirks.cpp`
- `Source/WebCore/platform/SourcesGStreamer.txt`
- `Source/WebCore/platform/graphics/gstreamer/MediaPlayerPrivateGStreamer.cpp`
- `Source/WebCore/platform/graphics/texmap/coordinated/CoordinatedBackingStoreProxy.{cpp,h}`
- `Source/WebCore/platform/network/soup/SoupNetworkSession.cpp`
- `Source/WTF/Scripts/Preferences/UnifiedWebPreferences.yaml`

These files are part of the upstream WebKit project and are licensed under
WebKit's own mix of **LGPL-2.0-or-later** and **BSD-2-Clause** licenses
depending on the file/subsystem. Since these patches only exist on the
build server and are not currently checked into this repository, they are
listed here for completeness and compliance tracking — the same "corresponding
source" obligation described above applies to them.

## Summary table

| Path | License | Notes |
| --- | --- | --- |
| `wpe-drm/WPEViewDRM.cpp` and other WPEPlatform DRM backend files | BSD-2-Clause | Mirror of upstream WPE WebKit file |
| `wpe-drm/GStreamerHolePunchQuirkRockchip.{cpp,h}` | LGPL-2.0-or-later | Original quirk, using WebCore file header/license |
| `assets/wpe-runtime/lib/librga.so.2` and minimal IM2D ABI declarations | Apache-2.0 | Rockchip RGA runtime and header-compatible ABI declarations |
| `wpe-drm/webkit-patches/*` (patches, not vendored here) | LGPL-2.0-or-later / BSD-2-Clause (per upstream file) | Applied only on build server; see `wpe-drm/webkit-patches/README.md` |
| Everything else (`src/`, `jsapi/`, `scripts/`, `tools/`, etc.) | MIT | See root `LICENSE` |
