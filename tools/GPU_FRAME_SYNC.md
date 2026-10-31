# GPU Frame Synchronization Regression Notes

## Scope

This change fixes the DRM consumer and producer synchronization boundary;
it does not change page zoom, memory policy, refresh-rate settings, or the
CPU/GPU selection policy. Production changes replay as
`webkit-patches/0650-gpu-fence-and-scanout-coherence.patch`.

## Confirmed Code Defects

- `IN_FENCE_FD` was calculated but omitted from the atomic request. Direct
  scanout now submits it; hardware without the property waits before commit.
- A positive `poll()` result was treated as success even for `POLLERR`,
  `POLLHUP`, or `POLLNVAL`. Those events now reject the frame.
- Rotated frames waited up to two seconds inside the input/main thread.
  `RenderingFenceWait` now uses an FD source and an absolute deadline. It is
  cancelled before the view releases its FD or pending buffer.
- Dropped buffers sent ReleaseBuffer but not FrameDone, potentially stopping
  the compositor. Discards now complete the producer without advancing the
  displayed generation.
- RGA cache-sync failures were ignored. Failed synchronization now prevents
  submission. RGA compound blits explicitly request `IM_SYNC`, as defined in
  the [Rockchip headers](https://github.com/airockchip/librga/blob/main/include/im2d_type.h).
- Cursor, chrome and webpage commits share the in-flight exclusion; pending
  chrome scanout buffers cannot be reused merely because pendingBuffer is null.
- Every toolbar-visible frame cleared the whole destination and zeroed the
  base vector before overwriting them. Only uncovered margins are cleared;
  base storage retains its size between complete copies.

## Validation

- GLib/eventfd/pipe tests cover readiness, error plus readability, invalid FD,
  timeout, cancellation and main-loop responsiveness. Pixel tests verify that
  margin clearing preserves page pixels and stride padding in all orientations.
- Nine native ASan/UBSan suites pass. The complete patch series was replayed
  independently and compared with the incremental compilation worktree.
- ARM64 X7, Mali-G52, 960x266, rotation 270: tested the local scrolling fixture
  and `https://github.com/marketplace` with 45 pairs of 120ms swipes.
- GitHub windows containing at least 40 presented frames: weighted copy mean
  was 11.27ms before and 10.52ms after. Frame-rate ranges were 23.3-41.5 and
  27.9-58.2 respectively. Fresh test Profiles were used; network/page content
  variation means these ranges are observations, not a guaranteed speedup.
- The fixed GitHub run recorded 1500 asynchronous fence waits, maximum 29.19ms,
  with no reported fence failures. Average FPS alone does not prove the absence
  of occasional visual tearing; retain real-page visual regression testing.
- Local auto-scroll remained approximately 30fps on both GPU and CPU. The
  zero-rotation, chrome-disabled GBM scanout diagnostic also remained about
  30fps without commit errors. That debug configuration still emits existing
  chrome-base-deferred messages; it is not the normal toolbar-enabled path.
- Runtime SHA-256: `23e2dc3d4a7f7e201b514719994db4a76ddc244848db17ebd9f849bad50754b6`.
  GPU AMR SHA-256: `a1866ac4216da5493263dec3a4490005e46eb27bc3097dce97ae1cbfaadaaca3`.
  Before-change GPU package is retained under `.rollback/packages/` with SHA
  `ce65fb19a0715c4b076699154c16fd4a068ed43421de536425e576c3d07dad60`.
- Installed on the connected ARM64 X7 (`miniapp_cli install` returned 0).
  The installed slot-b WebKit hash matches the runtime above. All seven
  pre-existing tabs remain in the user database; launcher is left at index,
  with no WPE child processes or browser-owned GPU modules running.

## Reproduce And Avoid Regressions

Use `tests/fixtures/web/gpu-scroll-stress.html`. For the WebGL workload, serve
with `?webgl`, or copy it as `gpu-scroll-stress-webgl.html` for direct file
loading. Do not rely on query-string file URLs with the launcher's HTML loader.
Run `npm test` and `npm run test:sanitizers`; build/ABI/manifest checks must
precede installation. Use a separate WPE_VAR_DIR for diagnostic launches,
preserve real Profiles, and stop test processes and owned modules afterward.
Do not restore blocking fence waits, ignore cache-sync errors, or replace
page-flip ownership with fixed buffer-retirement delays.
