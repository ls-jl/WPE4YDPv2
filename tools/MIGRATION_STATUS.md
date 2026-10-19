# Local Build Migration

## Baseline And Completed Repairs

- Existing `dev` commits and user files are preserved; no history rewrite or push.
- `.rollback/migration-1791208532938/before.json` records original metadata and
  hashes. Restored 41 materialized links and Git modes; also removed accidental
  group/world-write permissions on tracked files. Every touched file retained
  its content SHA256. No baseline ELF or AMR was rebuilt or replaced.
- Scripts now share `build-env.sh`. Default output is `/srv/wpe-build`, with
  compile/link/ThinLTO concurrency 6/1/2. Old server paths are not active defaults.
- Existing WebKit worktrees are preserved. JSAPI builds target staging.
- Added isolated CPU/private-GPU packaging and schema-2 source/toolchain/ELF
  manifest generation. Legacy schema-1 manifests remain valid for inspection,
  but cannot qualify an old runtime as a freshly rebuilt package.
- Local fixtures pass for path overrides, target pkg-config relocation and
  refusing to remove a dirty existing worktree. Shell syntax and the original
  binary manifest checks pass.

## Verified Local Rebuild

- Host dependencies are installed and `/srv/wpe-build` is writable. Git LFS
  current objects pass fsck. Pinned Node 20.15.1/npm 10.7.0, package-lock and
  normal/offline npm ci are verified; the system Node installation was retained.
- Debian base sysroot was authenticated using a private keyring with official
  Debian 12 primary fingerprints (the distribution's 2021 keyring was too old).
  Downloads, checksums and APT state remain isolated from the host package DB.
- GStreamer core/base/good/bad 1.22.12, libnice 0.1.22, OpenSSL 3.5.7, libsrtp,
  dav1d and AVIF were built locally. GStreamer patch 0001 required restoring
  missing blank context lines to apply exactly to the official release.
  ICU72 baseline libraries and JPEG8 2.1.5.1 have an explicit ABI overlay.
- WebKit production patches replayed in a fresh worktree; the resulting DRM
  mirror hash matches the repository. Existing worktrees can only be reused
  with matching input AND source-content fingerprints. Original source edits
  and old build directories have not been overwritten.
- JSAPI and GPU probe/shim rebuilt as AArch64 and pass ABI limits. The new JSAPI
  SHA256 is `7ec6568c447ac2c7e36865204c70605e4cbe87f1414846061b87168e88913a0d`.
  WebKit, WebProcess, NetworkProcess and launcher finished a complete local
  build. The new WebKit SHA256 is
  `f0f245a29bb9cbad803fbc45b24792cdf186afe03ce52249de723afed69ed03b`.
  Native artifacts were promoted only after ABI, closure, symbol and source
  checks; previous native files remain in `.rollback/native-before-local.74mJh7`.
- A full closure scan found pre-existing missing codec/MPP libraries. Bundle
  assembly must provide real SONAME files, not merely find SONAME strings in
  another filename. The new closure fixture covers this distinction.
- `npm test`, isolated MiniApp dev build and eight ASan/UBSan suites passed.
  This host needed non-PIE sanitizer executables after intermittent PIE startup
  failures. The CLI's dev build also deletes its cwd AMR; dev builds now run in
  isolation, and the baseline AMR was restored from verified rollback bytes.
- The complete bundle passed checks for 319 AArch64 ELF files and required
  symbol resolution in seven entrypoints for both CPU and Mali profiles.
  libgudev and the SDK-matching versioned xkbcommon provider were also bundled.
- CPU AMR: `8001779591038449.1_0_0.amr`, SHA256
  `f7bbeb6b06f92ed5d651fa12acb6690fafc149e2aa701f7aa04d9012996c2f06`.
  Its 314 native archive members match the manifest; the public policy passes.
- Private GPU AMR: `.cache/releases/gpu/8001779591038449.1_0_0.amr`, SHA256
  `f20a339a4719bc16c4d2b60479bc16c91ba94d7e1c1c11522ff6e2f4f33a8f22`.
  Its 323 native archive members match; the public policy rejects it as intended.
- Remaining validation: real display/rotation, keyboard, video, performance,
  lifecycle and kernel-module behavior. No install or cloud-game session was
  performed. The latest ADB identification was Buildroot RK3562 Y07, aarch64,
  kernel 5.10.160, with miniapp_cli and libstdc++.so.6.0.29. Its ADB model label
  says Nexus_4 but it is not an Android phone; identification used OS/architecture.
- PGO remained enabled. A small number of changed functions emitted profile
  hash-mismatch warnings, so their outdated counters were discarded. No claim
  of measured device performance is made from compiler success.

## Resume Checklist

1. Enable the pinned tools with `source scripts/build-env.sh`.
2. Keep current sources, SDK receipt and artifact hashes together; do not accept
   an unknown cache simply because a library file exists.
3. For another build, select fresh staging paths and preserve previous outputs.
   Source/worktree mismatches must stop rather than force-remove a worktree.
4. Run `npm test`, sanitizer fixtures and ABI/symbol/AMR checks after changes.
5. Complete device validation on the identified ARM64 pen without consuming
   unbounded cloud-game time; do not present local checks as hardware acceptance.
