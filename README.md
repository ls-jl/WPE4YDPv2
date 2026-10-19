# Matched G17 Kernel Source Companion

This generated source-only snapshot contains the 15 kernel/driver patches, exact
original/built configuration, unmodified build controller/policy and notices for
the experimental `5.10.160-pen-managed` candidate. No KO, Image, firmware, Mali
userspace blob, compiler payload, credential or device log is distributed here.
Do not edit this generated copy as a second development tree.

## Scope And Licenses

The base is the publicly accessible Rockchip non-Android kernel at commit
`acdf68dc443a7c9a68be769abc6cf4d85617f283`, tree
`b43acf827e54177453b8097668356a9ae4fa2c03`:
https://github.com/rockchip-linux/kernel/commit/acdf68dc443a7c9a68be769abc6cf4d85617f283

`PUBLIC-UPSTREAM.json` records the official API identity and unsigned signature
status. `SOURCE-MANIFEST.json` binds inputs, patch order, per-file notice review,
preferred-source inventory and actual build artifact hashes. Original notices
remain unchanged. OPP files/new header are GPL-2.0-only; Rockchip OPP selection is
GPL-2.0-or-later; platform files grant GPL version 2. Some ARM files declare the
Linux syscall exception. `mali_kbase_devfreq.c` retains two differing SPDX lines:
this companion relies on GPLv2 and **does not assert an exception for that file**.
No exception is extended to other files. Full GPL/exception texts are in notices/.

The root `LICENSE` covers original project Python tooling only (MIT), not kernel
patches. This is public source material, not approval of any kernel binary or
browser package. A future binary distributor must provide the complete matching
corresponding source through an appropriate distribution method, not merely this
patch pointer or a promise that a third-party URL will remain available. The
separately prepared complete source bundle is not relabeled as publicly delivered.
Mali userspace licensing, other dependencies, patents and sandbox are separate.

## Obtain And Verify

Use Linux, Python 3.12+, Git, make, host cc, bc, flex, bison and modinfo. Keep the
source companion and build/source directories separate. Verify the source branch
commit through a trusted channel, then run:

```sh
KIT="$PWD"
sha256sum -c SHA256SUMS
python3 -B chromium/scripts/export_g17_source_companion.py verify --directory "$KIT"
```

In a new directory outside this companion, fetch only the fixed public commit:

```sh
WORK=/absolute/path/to/new-g17-reproduction
mkdir "$WORK"
git init "$WORK/upstream"
git -C "$WORK/upstream" fetch --depth=1 --no-tags https://github.com/rockchip-linux/kernel.git acdf68dc443a7c9a68be769abc6cf4d85617f283
git -C "$WORK/upstream" checkout --detach FETCH_HEAD
git -C "$WORK/upstream" rev-parse HEAD HEAD^{tree}
mkdir "$WORK/build"
```

Confirm both printed IDs equal those above. Obtain the original Arm GCC10.3.1
archive from:
https://developer.arm.com/-/media/Files/downloads/gnu-a/10.3-2021.07/binrel/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu.tar.xz

Its SHA256 must be
`1e33d53dea59c8de823bbdfe0798280bdcd138636c7060da9d77a97ded095a84`.
Set `GCC_ARCHIVE` to that archive and `GCC_ROOT` to a separately extracted
`gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu` directory. The controller
checks every compiler archive/tree member; it does not download or silently
substitute a compiler. At least 8 GiB free space is required before building.

## Rebuild Without Installation

```sh
python3 -B "$KIT/chromium/scripts/build_g17_kernel_candidate.py" \
  --source-root "$WORK/upstream" --config "$KIT/config/original.config" \
  --gcc-root "$GCC_ROOT" --gcc-archive "$GCC_ARCHIVE" \
  --build-root "$WORK/build" --jobs 6
```

The unchanged controller archives the clean upstream, applies all 15 patches and
builds Image/modules with normal modpost and managed-import checks. Compare the
new receipt's artifacts with `SOURCE-MANIFEST.json`; the shipped built config is
the expected result, not a replacement for the original input. Record actual
results: matching SHA values are expected inputs, not a claim that a new build or
another host has already passed. Do not reuse an unknown old output directory.

This kernel/module introduces three OPP exports absent from stock X7/#5. Never
force-load it on that stock kernel, change vermagic to hide the difference or
unload system modules. No DTB/boot image, recovery/flash procedure, universal KO,
hardware safety or four-direction/browser acceptance is supplied here. These
commands never install, insmod, flash, modify a device or touch WPE/user data.
