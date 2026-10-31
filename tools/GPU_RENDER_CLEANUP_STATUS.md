# GPU Render Cleanup Delivery Record

Baseline: `dev@150b4b53c2bf8530b198d4ce652abda06d27908d`.
Implementation branch: `refactor/gpu-render-cleanup`.

## Batches

| Batch | State | Evidence |
| --- | --- | --- |
| Baseline and diagnostics | Complete | `40b00c9`; 741-file hash-verified snapshot, private GPU AMR copied separately; barcode checks cover four rotations, 0/44/80px inset and mixed frames |
| Single source and module boundaries | Built and tested | `c0adb99` removes duplicate implementation; 46-file byte equivalence passed. Geometry, scanout, queue, chrome, video and WPE orchestration compile separately in the final ARM64 runtime |
| Fence, CPU access and frame ownership | One-SKU regression passed | Bounded fence quarantine, explicit frame phases, scanout write exclusion and paired CPU access integrated; 13 ASan/UBSan suites, four-angle owned-pixel checks and 15-minute GPU test passed |
| Video release and shutdown | Controlled local media passed | Shared v3 wire, ordered release/close ACK, nonblocking producer and failed-disable retention; real NV12 probe and H.264 MPP playback passed. Remote WebRTC A/V not accepted |
| Chrome cache and redraw performance | One-SKU measurements | Immutable view snapshots and lazy source-base rebuild; static-page Tabs drag ~35fps. GPU page preparation ~2.2ms versus ~9.8ms baseline. Physical tearing requires separate evidence |
| Redundant code and runtime aliases | Packaged and installed | Startup-only rotation, real viewport resize, full copying and read-only validation; 76 aliases removed with dependency checks. Public CPU/private GPU package checks passed |
| Build and device acceptance | Partial hardware acceptance | Frozen native inputs, local 6 compile / 1 link / 2 ThinLTO build; JSAPI/runtime ABI/provenance, AMR and LFS checks passed. Latest GPU AMR installed. Second SKU offline |

## Evidence Rules

- Frame source, copied scanout and presented buffer have distinct identities.
- A rendering-fence timeout does not authorize recycling the GPU source.
- Trace buffers are bounded; default logs contain aggregate timings only.
- Pixel checks use an owned, completed buffer. Reading a live reused DRM FB
  can itself mix frames and is not evidence of physical display tearing.
- Benchmark active scrolling separately from static-page idle intervals.
- Device logs, captures and private GPU packages stay outside Git.
- Preserve user Profiles, cookies, tabs and other workspace changes.

## Validation Workloads

Use `tests/fixtures/web/frame-integrity.html` for barcode coherence and
`gpu-scroll-stress.html` for composition load. Use `native-drag-regression.html`
for prevented touch, ordinary scrolling and keyboard request regression.
Real-page checks cover ThePaper article 31789106, Baidu weather search and
GitHub Marketplace. Use controlled local WebRTC streams, not paid cloud games.

Each batch records its implementation commit, tests, native build and device
status here. Test success is not hardware acceptance.

The intermediate module build was stopped after object compilation began:
its old recipe captured source hashes only at the end of a build. No artifacts
from that stage were promoted. Final build inputs must be frozen and checked
against their start-of-build snapshot before creating a manifest.

## Current Implementation

- `DRMFrameQueue` manages acceptance, latest-frame queuing, presentation and
  release bookkeeping; `FrameTicket` rejects invalid phase transitions and
  repeated/stale completion. Page generations advance on presentation only.
- Unsignaled discarded sources move to a four-entry asynchronous quarantine.
  Error fences stay retained until channel teardown, and swapchain allocation
  is bounded. Frame pacing completion cannot authorize producer reuse.
- `DMABufCPUAccess` pairs START/END across target copy, restore, raster and
  video alpha writes. Failed END retains access ownership and prevents commit.
  Submitted/presented targets reject writes.
- Video protocol v3 has a shared sized wire structure, sequenced release and
  close-complete ACK. The producer retains samples after disconnect; the
  consumer retains current scanout after failed disable. Busy video commits
  yield to the CRTC queue instead of sleeping in the input loop.
- Chrome snapshots belong to each view and are immutable copies. Native
  redraw sources exist only for requested changes or active animation. INI is
  diagnostic output; mouse/menu redraw no longer uses fake viewport resize.
- Stable visible toolbars do not request full CPU page-base snapshots. A
  clean base can be rebuilt from the retained completed WPE source, even for
  static pages after cache eviction. Geometry changes invalidate old bases.
- Full scanout copying remains mandatory. Hot rotation files, legacy inset,
  partial-copy bookkeeping, unused lifetime totals and VM mutation are gone.
  Runtime library validation is read-only; normalization happens in staging.
- Two-second percentile statistics report frame gaps and latest raw-input to
  presentation delay. This is not a DOM acknowledgement measurement. Use
  `WPE_DRM_BENCHMARK_ACTIVE=1` in continuous-motion tests so long gaps are not
  filtered as idle. `WPE_DRM_FRAME_TRACE=1` keeps 256 bounded trace points and
  dumps them on teardown; no input text or URL is included.

The source checks passed for all six DRM translation units and 13
ASan/UBSan suites. Media receipt schema 2 was migrated only after proving its
baseline build recipe byte-identical and checking all source/archive and
788 artifact hashes. Source inputs are now captured before native compilation
and checked again before a manifest can attest the output.

## Device Evidence (2026-10-06)

One ARM64 pen (`MEB0400008900794`, RK3562/G52, DRM 480x960) is online.
Tests use `/userdisk/wpe-gpu-cleanup-test` and disposable Profiles, not the
installed browser's database. No paid cloud gaming is used.

- First full local build, JSAPI provenance, runtime ABI/dependency closure,
  public CPU AMR and private GPU AMR checks passed. `npm test` passed after
  generated supervisor synchronization. `aiot-cli check` only recommends
  optional global `cnpm`; npm builds succeed without installing it.
- Frozen frame 150 has identical top/middle/bottom barcodes at output 90/270
  (960x222 content + 44px chrome) and output 0/180 (266x880 + 80px chrome).
  This checks owned pixels, not physical panel scanout.
- Real DRM NV12 probe passed: no early release of current sequence 1,
  replacement released 1, hide released 2 before CloseComplete sequence 3.
- Same-device GPU scroll workload: baseline prepare ~9.8ms; cleanup ~2.2ms;
  both sustain ~30fps. Cleanup CPU workload also sustains ~30fps. GPU fence
  failures are zero in these runs; RGA import reuse is observed.
- Portrait overflow geometry is 2 columns by 3 rows; opening a menu on the
  frozen page reconstructs the clean base and accepts native hit testing.
- User power exit completes and WPE children exit. One static-page SIGTERM
  timeout required forced termination: the launcher still called GLib from a
  POSIX signal handler. `5c211a6` replaces this with GLib signal sources;
  startup shutdown is also remembered before the main loop exists.
- GPU first-frame health is armed after document commit, not during DNS/HTTP
  waiting. A 20-second local response delay now renders with Mali without a
  spurious CPU restart. The temporary server address changed during the user
  interruption; stale-address/error-page runs are not successful page tests.
- A concurrent installed browser was detected during early touch injection.
  Overlapping real-site results are excluded. Subsequent isolation used a
  disposable hole-only MiniApp (appid 8001779591038450); capture alpha was zero
  for all 255,360 pixels. This firmware needs `miniapp_cli start APPID index`,
  not a literal `--index` page argument. The diagnostic app is removed at end.
- Actual H.264 MPP playback ran through protocol v3 for ~960 frames, including
  a loop: committed/ACK failures=0, producer inflight=2, consumer inflight=1,
  commit average ~9.3ms and maximum ~17.4ms. Local DataChannel echoed ten
  messages each way with ICE completed/completed. This is not a remote A/V
  WebRTC or cloud-game acceptance result.
- Final 900-second portrait GPU composition run: 443 stable windows,
  FPS min/median/max=28.9/30.1/30.6; prepare median=1.78ms, commit median=0.10ms;
  fence failures=0. FD counts stayed 33/42/23. WebProcess RSS was ~172MiB early
  and ~162MiB late, with no process swap or unbounded import growth observed.
- Ten CPU/GPU startup/shutdown cycles covered all output angles. Child exit
  is asynchronous: allow a bounded grace, not an instantaneous `pidof` check.
  All ten cleared within 0-1 seconds after supervisor completion; no owned GPU
  modules remained. A first harness incorrectly treated timeout's expected
  124 result as failure; a second exposed this short child-exit grace.
- Latest installed MiniApp restores seven user tabs from the same Profile,
  runs WPE from the package and has fullscreen alpha-zero hole. Address and
  Baidu input both open the learned global keyboard; cancel keeps WPE alive.
  Cached launcher return was found broken because hooks were at Vue's top
  level. `9daa7dc` moves them into methods; a production-component regression
  failed before the fix and passes after it. Real power exit now shows Ready
  and "浏览器已关闭", with no WPE children or browser-owned GPU modules.

## Delivery And Limits

- Native/source/package commits stay on `refactor/gpu-render-cleanup`; no
  merge into dev, force push or history rewriting was performed.
- Public CPU AMR is the root tracked artifact; private GPU AMR is under
  ignored `.cache/releases/gpu`. WebKit SHA256:
  `9ffcde7f66c39c84e7ea65dfa6cf3f96df90caef96d2bb0a3a42c9a06b244bc5`.
  Launcher SHA256:
  `9e2862eab407c2d0f409e11fda9bc22afc11d497ab15c170a1311303f4c20672`.
  JSAPI SHA256:
  `ae690d6f2527d2625b52e430e8a4b5169ae08ca49faed021b45a64806e53ed69`.
- Frozen builds 2-4 differ only in launcher source. Completed WebKit inputs
  and binary hashes were checked before launcher-only reuse; its ELF stays
  identical. Do not use SKIP_WEBKIT_BUILD with unknown earlier outputs.
- Backups, private logs, captures and user site data remain local and ignored.
  Seven tabs and the user's Cookie/site data are preserved; historical user
  runtime/cache directories are deliberately not deleted.
- Final installed private GPU AMR SHA256:
  `77e7525be4d688b8213f556bbda53a10f96cd091d15b2fd69d271ffeca9e370a`.
  Public CPU AMR SHA256:
  `c1b33a1394cd3a361d655baa8ee68c1522f0eaa5dfbb1934d1e4455b3fc718e3`.
  All final tests, native manifests, archive policy and current LFS objects
  pass. Source/artifact batches include `bde4aab`, `9daa7dc` and `b9729dd`.
- Final power-return capture shows Ready, a usable start button and the closed
  status, not Starting. Browser, WebProcess, NetworkProcess and owned GPU
  modules are stopped. The fixture server, temporary hole app and device test
  directory have been removed; user data remains one Profile/seven tabs.
- Pending: second SKU, textarea-only keyboard hardware, remote WebRTC A/V,
  physical scanout tearing evidence, sustained real-site A/B repetitions and
  hardware critical-pressure static-page cache recovery. GitHub did not load
  reliably in the current network; do not count its error page as acceptance.
  Do not describe these source changes as a universal tearing fix.
