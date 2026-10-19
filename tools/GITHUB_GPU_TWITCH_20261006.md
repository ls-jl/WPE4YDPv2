# GitHub GPU Twitch Investigation

## Captured Incident

- Page: `https://github.com/ls-jl/WPE4YDPv2` on pen `MEB0400008900794`.
- One launcher, Mali GPU, panel 960x266, output 270, DRM envelope 480x960.
- User reports both old-position jumps and local white/ghost blocks.
- Before log: median 58.7fps, prepare ~1.1ms, atomic call ~0.1ms, no fence errors.
  FPS alone does not establish pixel correctness.
- A retained scanout capture shows repeated/overlapping README heading glyphs
  and blank areas. WPE was paused ~100ms with an EXIT/TERM/INT resume trap so
  the captured BO could not be reused mid-read. This is buffer evidence, not
  a physical panel scanning measurement. Private capture/logs stay in `.cache`.

## Concrete Source Hazards

1. Partial accelerated tiles use `glCopyTexSubImage2D`, then clear the pending
   update references. The shared texture pool reuses entries at refcount 1.
   `serverWait()` orders the painting write but does not complete the later
   compositor read; another GPU worker can acquire its source too early.
2. Skia paints the whole layer tree, not a target-damage-clipped subset, while
   opaque targets are only cleared on initialization or async-scroll reasons.
   Animation/rendering updates can keep previous footprints in uncovered
   regions. Existing opaque checkerboard workloads masked this condition.
3. Raw GL tile uploads intervene between Ganesh calls. Reset its backend state
   cache before the next composition rather than trusting stale bindings.

## Fix And Guardrails

- Patch `0710-skia-tile-upload-lifetime.patch` retains partial-upload buffers
  behind an exported read fence, with at most four pending batches per store.
  Unsupported export uses synchronous GL completion before releasing refs.
- Reuse the production rendering-fence helper. Teardown must complete reads
  within its deadline or stop the unsafe process; errors do not authorize reuse.
  Ready fences close immediately, including static stores that have no later
  update to reap entries.
- Clear GPU composition targets and publish full output damage because Skia
  already traverses the entire scene. CPU raster behavior stays unchanged.
  This is not repainting every DOM tile and does not lower refresh frequency.
- Add production-helper timeout/error/ready/FD regression tests and the
  `partial-tile-stress.html` workload. Keep patch replay, source snapshot,
  ABI/manifest checks and public CPU/private GPU package separation.

## Verification Status

- Ownership tests and all 13 ASan/UBSan suites pass.
- First build stopped at an allocator-tag check; the nested helper now uses
  WebKit's TZone allocator. Failed staging outputs were not promoted.
- Independent replay, ARM64 compilation, ABI/manifest, CPU/private GPU package
  checks and installation pass. Installed WebKit SHA256:
  `7d5e72a8aeca533faa65389608622d1caadabe601be1c0c529627b1c081e582b`.
- The user initially confirmed normal rendering; a later report identifies
  occasional persistent white regions and short drags accumulating after a
  stall. A clean retained capture is not proof these residual issues are fixed.
- Preserve current user tabs, Cookie data and page until verified replacement
  is ready. Never overwrite a library mapped by a running WPE process.

## Residual Issues, 2026-10-07

- Whole-tile updates still swapped GL identities between pool objects while
  Ganesh borrowed the old texture without a release callback. Partial-upload
  fences alone do not cover this ownership path. Patch 0730 replaces the tile
  reference without swapping IDs and retains the borrowed pool entry through
  Skia's texture release callback, including tile removal and replacement.
- WPE wheel coalescing ignored scrolling phases, allowing separate short drags
  to merge behind a DOM acknowledgement. Patch 0720 keeps phase boundaries
  separate and dispatches touch begin/end boundaries without that queue wait.
- The pinned source disabled all offscreen tile coverage. Bounded vertical
  prefetch restores up to half a viewport/one tile row, with a 4MiB extra-row
  budget reduced to 2MiB under pressure. This addresses late coverage only;
  persistent white is not classified as a prefetch failure without evidence.
- A Douyin tab separately exceeded its WebProcess memory ceiling
  (494MB over 491MB) and loaded the internal `about:blank` error document. Its
  stopped frame count must not be used to diagnose GitHub tile corruption or
  justify removing the memory limit. User data and tabs remain untouched.
- Post-patch ownership and wheel functions are compiled in API-stub regression
  tests; these establish reference/phase rules, not GPU pixel correctness.
  New native build and retained-buffer/device tests remain required.

## Second Build And Device Checks

- Source commit: `f06585b`. Independent replay, native build, `npm test`,
  production-function ASan/UBSan and 14 native sanitizer suites pass.
  `aiot-cli check` only recommends optional global cnpm.
- WebKit SHA256:
  `27a0af8de33bb04c613c974899b0770309471ca0ab470ce467f348a98caa130c`.
  CPU AMR:
  `88ac8f1c491bc55bf183636d9b35127c858631756a8deb3b4a80c715ddfef514`.
  Private GPU AMR:
  `0640a537deca57d95dbb2346a2683e98e781be50ae9465dc5b411bd4685b9c68`.
- The private package is installed in slot `a`; launcher maps and device ELF
  hashes match the build. It restores eight tabs and active GitHub tab 35,
  uses Mali, and leaves Profile/Cookie data unchanged.
- The isolated 600-frame ownership workload sustains median 30fps versus
  30.1fps before the fix. The frozen owned portrait buffers have four complete
  text/color columns; the largest near-white square below chrome is 1px,
  not a missing tile. Both tests pass, so this is regression evidence rather
  than a reproduced before/after corruption fix. Portrait chrome differed
  because physical touches hid it in the baseline run.
- Portrait and rotated landscape GPU runs reach frame 600 with zero fence
  failures. Short GitHub drags also have zero fence failures, but initial
  post-navigation windows include 39-44fps and up to 64ms latest-input P95.
  They are not comparable to the warmed 48-49fps baseline windows; do not
  claim a real-site latency improvement from these samples.
- Direct isolated Guest startup crashed before its first frame, whereas an
  isolated persistent Profile completed. Keep this as a separate unverified
  compatibility issue; failed Guest/master harness starts are excluded from
  GPU pixel acceptance. No user data was used for those tests.
- `/home` filled during packaging. The first GPU package failed and was not
  installed. Two locally generated native rollback directories were moved
  to `/srv/wpe-build/rollback` with original-path symlinks; all backups remain.
  Retried packaging, integrity checks and installation passed.
- Pending: repeated long real-site white-region reproduction, physical
  scanout proof, a second SKU, and detailed Guest-startup diagnosis. This
  batch does not establish that every persistent white region is eliminated.

## Persistent White Regions: Source-Side Isolation

- Tile copy-on-write and producer fence retirement (0740) built into WebKit
  `4c5ad926e6a0fac6578e49b4ec870200064267044beb4b48cb68ea3c3955ce54`.
  Ownership tests passed, but the user still reproduced white README content.
- Both retained source DMA-BUFs exactly matched the simultaneously retained
  scanout below chrome/progress: zero differences in 699,048 color channels.
  The persistent blank region was already present in the WebKit source image,
  not introduced by RGA rotation or DRM target copying.
- The missing H1 remained visible, opaque and inside the viewport according
  to a one-time read-only DOM probe. The same Profile, desktop UA, URL and 0.75
  zoom rendered completely with CPU raster. No profile data was cleared.
- Disabling linear tile textures or GPU painting workers alone did not remove
  the issue reliably. One clean capture does not establish a probabilistic fix.
- Temporary CPU tile raster plus Mali compositor/WebGL no longer reproduced
  white regions in the user's round-trip test. The production GPU profile now
  chooses that compatibility combination, not pure CPU composition. Long-term
  and second-SKU acceptance remain outstanding. Do not claim pure GPU tile
  rendering is repaired, or compare unmatched mobile/desktop FPS samples.

## Touch Stall: Live Reproduction on 2026-10-07

- Native touch counters continued increasing while frame count stayed at 1758;
  MemAvailable was about 330MiB, WebProcess RSS about 285MiB. Main, scrolling,
  compositor and tile workers were idle, not stuck inside a GPU operation.
- GDB observed successful, unhandled DOM completions reaching the gesture
  detector, but its optional sequence remained 95 while current contacts were
  above 330. The detector rejects different sequences before processing DOWN.
- Clearing only that verified stale optional sequence in the live process
  restored native drag sequence 339 and rendering, without navigation/restart.
  This was a diagnostic experiment, never a production memory-patching method.
- Patch 0750 resets the detector on physical DOWN and rejects completion events
  outside the latest single-contact group, including stale DOWN and handled
  completions. Multitouch stays in the DOM path. Same-group late UP remains
  valid; neither a two-entry retirement range nor restart is the recovery rule.
- Regression tests compile the production completion gate and cover old DOWN,
  hundreds of new groups, cancel, multitouch and native-scroll retirement.
  Final rebuild/package/device validation is recorded separately below.

## Published Compatibility Build and Further Input Findings

- WebKit `e268829c829ce1985447ec2075879fcd8a6540d47bdd4c49484d5584459cb1c6`
  passed independent replay, ABI, closure, manifest and package verification.
  All repository fixtures and native ASan/UBSan suites passed. The first build
  wrapper reported 143 after writing the completed stage; a fresh launcher/stage
  run exited 0 with identical WebKit bytes before release verification.
- Real MiniApp startup restored Profile 1, all eight tabs and active tab 35.
  Installed launcher was the production binary, not the DOM diagnostic program.
  GitHub's first request stalled; the next session reached LoadFinished. Its
  retained portrait capture shows the previously missing README heading, links
  and introduction fully painted. This is not exhaustive site acceptance.
- Eighty-nine mixed short/long gestures on the isolated native-drag fixture
  completed without a stale-sequence freeze; the final stroke still scrolled.
  Real-site timing was excluded once unscripted touches overlapped injection;
  the user confirmed simultaneous physical interaction.
- GPU tile ownership animation reached frame 600 at about 30fps, with complete
  retained four-column contents. Typical RGA was 0.8ms, page preparation 1.9ms,
  commit 0.1ms and fence failures zero. GPU portrait/270-degree and CPU portrait
  barcode captures all decoded frame 150 consistently in three bands. These
  are owned-buffer checks, not physical scanout measurements.
- WebGL2 clear/readback returned 64,128,191,255 with error 0. Native EGL probe
  identified ARM/Mali-G52; the webpage renderer string reported Apple GPU and
  was not used as hardware-identification evidence.
- Overlapping contacts exposed another decoder gap: replacement could lose
  the old UP/CANCEL, while BTN_TOUCH also rewrote MT slot zero. The launcher now
  selects MT tracking from evdev capability, cancels before replacement, and
  resets on SYN_DROPPED/disconnect. It preserves known coordinates and seeds
  them via kernel queries because evdev suppresses unchanged axis values.
- Device interruption probe observed DOWN 1, CANCEL 1, DOWN 2; subsequent native
  drags 2 and 3 worked. Production-callback fixtures cover replacement in slot
  one, same-packet UP/DOWN, duplicate tracking, MT/legacy, dropped sync and close.
  Launcher SHA256:
  `f87ba999fd8f23847a163c908c454135671c00928c77cc31f9b43b6157f8c89d`.
- One-MOVE drags exposed a remaining phase bug: dragBegin stays set on UP, so
  the old priority emits another Began instead of Ended. Patch 0760 gives UP
  priority; production phase-expression tests cover both begin-flag values.
  Its final rebuild/device checks are recorded below.
- All test sessions and the hole-only test app were stopped/removed. Eight tabs
  remain; no WPE children or browser-owned GPU modules remain. No cloud-game
  minutes were consumed. Long-term, second-SKU and full rotation matrix remain
  unverified; do not infer them from this one-device compatibility test.

## Final Short-Drag Release

- Patch 0760 rebuilt into WebKit
  `c21f52147ec9ae082da3fa719e8f1cd5e5646eec855c01d04db1f4698de98811`.
  Launcher remains `f87ba999...`, with evdev resynchronization. Full `npm test`,
  ABI/closure, packaged manifest and single-WebKit checks passed.
- Public CPU AMR:
  `b3ce392beac43480dba0ada9ac01289a692a2dfb57c71905f9c588bd889b2cf1`.
  Installed private GPU AMR:
  `b3b04f2612b8a564de434c4ca4c343089fb31cc1e73c28a3c1bd6efc24880095`.
- MiniApp initially retained the old workspace after reinstall. Home/re-entry
  resolved slot `a`, then production preparation restored executable permissions.
  Do not use a cached old slot or manually bypass preparation as release proof.
- Real interruption probe retained coordinates on replacement, emitted CANCEL
  before the new DOWN, and the one-MOVE drag logged native Began exactly once
  instead of twice. A following 18-MOVE drag scrolled correctly. No concurrent
  physical touch was used for this final acceptance.
- Eight tabs and the same active tab survived startup/exit. Test host removed;
  WPE/WebProcess/NetworkProcess and browser-loaded Mali modules stopped. Device
  left at the launcher. Long-term real-site and second-SKU checks remain pending.
