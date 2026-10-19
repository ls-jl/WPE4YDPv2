# Portrait Scroll And Tile Coherence

The 2026-10-06 regression covers thepaper.cn article 31789106 and Baidu
weather search on an ARM64 X7, Mali-G52, 266x960 panel, rotation 0.

## Changes

- Browser contacts use increasing IDs and kernel monotonic timestamps.
- Each contact captures its content origin at DOWN. Captured coordinates may
  leave that original viewport; toolbar resize cannot inject an 80px delta.
- After WebKit accepts a native drag, further moves go directly to the UI
  gesture controller. One DOM cancel ends that stream; delayed completions
  cannot undo the claimed gesture. Prevented touch interactions remain on DOM.
- Skia backing-store tiles wait for painting completion before reading a
  worker's texture/fence and invalidate cached image views after every update.
- Automatic toolbar changes are coalesced until contacts are up and quiet for
  120ms. The existing distance/velocity thresholds and 160ms animation remain.
  Rapid direction reversals cannot repeatedly resize the HTML viewport.
  Manual, menu and fullscreen operations cancel pending automatic work.
- Normal-pressure repeat reclamation uses a ten-second cooldown and the
  previous working-set high-water mark. Policy escalation, 32MB growth and
  system pressure bypass it; every process-kill check still executes.
- A 1GB device with at least 384MB available RAM and 128MB free swap can retain
  the balanced 512MB budget, instead of switching to 448MB because free swap
  narrowly misses 256MB. Tight-memory devices retain the conservative policy.

Production WebKit changes are in patches 0660 and 0670. Launcher geometry,
contact and toolbar helpers are tested in `native-scroll-state-test.cpp`.

## Evidence

- The initial article run repeatedly reclaimed 80-90MB and exceeded its
  process threshold. Frequent viewport/target changes were also recorded.
- Final article run: 65 seconds, 35 pairs of 240ms portrait swipes, one
  viewport application at startup, no process-memory termination. Active
  windows reached approximately 31-33fps; idle page FPS is not a benchmark.
- Baidu: 55 seconds with the same swipe sequence, native takeover for all
  70 contacts, approximately 26-33fps in the observed active windows, no
  process-memory termination.
- Fixture: one button click, 12 prevented pad moves with scrollY still zero,
  native scrolling outside the pad, one keyboard request on input click.
- Pure logic, ten native ASan/UBSan suites, ABI/dependency/provenance checks,
  CPU/public and local GPU AMR integrity checks passed.

Real-page network content varies. A short run cannot establish indefinite
memory stability or prove the absence of every transient visual artifact.
Framebuffer dumps made while scrolling can themselves mix frames; use settled
captures and check the active FB before/after a dump before diagnosing tearing.

## Test Discipline

Use a separate WPE_VAR_DIR and the existing uinput diagnostic. Stop and verify
all test processes before replacing any mapped library; an earlier temporary
run was invalidated by updating its library before shutdown completed.
Keep installed-package/user data separate, avoid cloud-game sessions, and
remove test processes, owned modules and temporary files after validation.

Final runtime SHA256: `a3691874939ddfa837bf7ad67b7dcb54f0c3faeb22182d0032353c72389c582b`.
Final GPU AMR SHA256: `c5bcacaeff43a9e2e98daf8cbd71da8f09399db4baa55a0f2943ea80c435b85f`.
