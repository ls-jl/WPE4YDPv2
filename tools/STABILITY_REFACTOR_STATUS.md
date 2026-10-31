# Stability Refactor Status

Baseline: `3f3d4d4` on `dev`. Rollback artifacts are private under
`.rollback/stability-3f3d4d4*/`; never publish this directory or its GPU AMR.

This is the historical audit record. Current implementation and acceptance
are tracked in `GPU_RENDER_CLEANUP_STATUS.md` on `refactor/gpu-render-cleanup`,
based on `dev@150b4b5`. Local ARM64 builds and source-bound manifests were
restored; the root AMR is the public CPU package. Thirteen native ASan/UBSan
suites pass on the current Linux host. Historical statements below about PVE,
baseline binaries and unavailable sanitizers describe their original batch,
not current build availability.

## Delivery Gates

1. Baseline hashes and failing regression fixtures.
2. Database recovery and MiniApp lifecycle ownership.
3. Keyboard backend, terminal events and frame acknowledgement ownership.
4. GPU/video release, fence scheduling and chrome cache recovery.
5. Memory, navigation and system defaults.
6. Component boundaries and reproducible public/private builds.

Each batch needs a separate commit and tests. Source-level checks are not
ARM64 build or device acceptance. Do not replace runtime binaries with an
unverified build, silently update manifests, force-push, or rewrite history.

## Verification Log

- Audit: existing `npm test` passed; isolated reproductions exposed stale
  startup rejection, rotation restart after stop, lost terminal keyboard
  text and the 512 MiB swap boundary. ADB was absent and PVE unreachable.
- Implementation and hardware status are recorded below as batches finish.
- Batch 1: verified rollback copy (670 regular files); async lifecycle tests
  reproduce and now reject stale startup failures and delayed rotation starts.
  SQLite lock failure no longer triggers recovery; confirmed corruption keeps
  DB/WAL/SHM evidence and allocates a new DEFAULT ID. Tab restore errors disable
  session saves. Local `npm test` passed. Launcher ARM64 rebuild is pending.
- Recovery follow-up: the new sidecar test proved SQLite itself reconstructs
  SHM during quick_check. Capture a private streaming snapshot before opening
  an existing DB, discard on a healthy/non-corrupt result, and promote only
  confirmed corruption. Full DB/WAL/SHM assertions pass with UBSan. ASan
  startup spun without output on this Mac and was terminated; retry on PVE.
- Batch 2: local keyboard tests now cover authoritative terminal/empty text,
  nonterminal global callbacks, reentrant close, page-local native objects,
  backend evidence, Shadow DOM writes and detached targets. Stale native ACKs
  no longer clear an in-flight command; failed writes complete as failed, and
  navigation expires web-input requests. `npm test` passed. Launcher rebuild,
  real textarea/global firmware timing, and per-process protocol IDs remain
  pending; no existing runtime binary or AMR has been replaced.
  Legacy global keyboards with no intermediate events are not classified as
  failed merely for being silent; only explicit startup failure enables retry.
- Batch 3: real 512 MiB swap-header boundary is balanced; critical pressure
  can recover to warning without first reaching the normal watermark. Invalid
  samples do not trigger recovery. Memory tests pass under UBSan. Supervisor
  defaults now disable cloud auto-click and expired-certificate exceptions,
  preserve system mute, and no longer cap intentional profile/GPU switches
  over the whole process lifetime. Full WebKit pressure unification is pending.

## Pending Work

- Completed locally: baseline snapshot; lifecycle cancellation; non-destructive
  integrity checks and recovery snapshots; tab-load write protection; keyboard
  final text/close/evidence and stale ACK fixes; swap boundary, staged pressure
  recovery and safer startup defaults.
- Delayed return-to-launcher navigation is generation-bound as well: hide or
  unload cancels it, and onShow resumes an already-requested exit without
  starting WPE again. Covered by the fourth async lifecycle fixture.
- Public archive gate: `verify:public`, upload and public manifest generation
  reject Mali/KO entries. Fixtures cover accepted and rejected archives. The
  existing tracked GPU AMR intentionally FAILS this gate; do not push it.
- Still pending: crash-resumable migration journal, posix_spawn helper,
  asynchronous process tasks, getBrowserStatus and module leases; process-level
  keyboard session identity and evdev resync; event-driven fence scheduling,
  video teardown ACK and chrome cache recovery; full policy unification,
  navigation/log redaction, module splits and in-memory chrome snapshots.
- Build work pending: isolated GStreamer replay, source/toolchain/dependency
  manifests, clean CPU-public/GPU-private packaging, full ABI checks and native
  rebuild. The current binary manifests describe the BASELINE, not edited
  launcher or other C/C++ source. No newly installable AMR was produced.
- Device work pending: both firmware keyboard backends, CPU/GPU, four display
  modes, touch, profile persistence, video and Home cleanup. No cloud game was
  launched or device package changed during this local work.

## Resume Order

Migration follow-up (2026-10-06): the local ARM64 toolchain and WebKit/media
build are restored, and these launcher/JSAPI fixes are now included in rebuilt
runtime/AMRs. See `MIGRATION_STATUS.md` for hashes, rollback locations and static
verification. The remaining functional refactors and device acceptance below
are still pending; rebuilding did not implement those unrelated refactors.

Keep the private baseline rollback copy; the root AMR has been replaced with
the separately checked CPU package. No history rewrite or push was performed.
Use the local build environment, then implement and
test the process/DRM ownership changes in separate batches. Do not modify
WebKit mirrors without a matching replayable patch and ARM64 build. Run
`npm test` plus sanitizer suites, compare all rebuilt artifact hashes, and only
then package/install on confirmed ARM64 devices. ASan remains unverified on
this host; UBSan profile/memory suites passed.
