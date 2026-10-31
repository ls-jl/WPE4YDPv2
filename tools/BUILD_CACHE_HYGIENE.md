# Build Cache Hygiene

## Cleanup on 2026-10-07

- Removed 30 obsolete experimental WebKit binaries/compressed copies
  (3,002,761,493 bytes), old Falcon output and the unused runtime-stage copy.
- Removed 24 completed or superseded native staging directories
  (8,312,077,643 bytes). Their manifests/input receipts remain under
  `/srv/wpe-build/build/cleanup-receipts`.
- Preserved source, SDK, active incremental build, patch replays, rollback
  packages, current public/private AMRs and the current tile-COW build.
- Git LFS remote verification could not confirm 467 historical objects.
  No LFS objects were pruned. The approximately 55GiB store moved to
  `/srv/wpe-build/git-lfs/web_browser`, with `.git/lfs` linking there.
  `git lfs fsck --objects HEAD` passed after relocation.
- Rollback packages and two recent native rollback directories also retain
  their original paths through links into `/srv/wpe-build/rollback`.
- Home free space increased from about 2GiB to 60GiB. The build partition
  retains about 143GiB free. Relocation frees home capacity; it is not deletion.
- Later touch/GPU debugging removed six completed CPU/GPU package stages
  (about 6.4GB). Per-stage hashes and manifests are in `cleanup-receipts`.
  Three additional native rollback directories were moved to `/srv/wpe-build`
  after full file/hash/mode/link comparisons, preserving original-path links.
  Other applications' disk usage is not part of this cleanup.

## Rules for Subsequent Builds

- Keep downloads, builds and large staging outputs under `/srv/wpe-build`.
- After verified promotion, discard redundant completed bundles; keep the
  current validated baseline and the next unpublished candidate.
- Keep build manifests and logs instead of retaining every complete runtime.
- Do not delete active source snapshots, SDK inputs, user data or rollback
  packages during ordinary cache cleanup.
- Never prune LFS history without remote verification. A successful Git push
  or small pointer file does not prove the binary exists remotely.
- Inspect `df -h` before copying runtime trees or producing packages.
- Cache deletion is not browser validation. The tile-COW candidate was installed,
  but pure GPU tiles still reproduced white regions. The CPU-tile/Mali-compositor
  compatibility profile and touch-completion fix require separate acceptance.
