#!/usr/bin/env bash
set -euo pipefail

ROOT="${PROJECT_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
PATCH_ROOT="$ROOT/wpe-drm/webkit-patches"
WEBKIT_BASE="${WEBKIT_BASE:?set WEBKIT_BASE to the clean WebKit git repository}"
WEBKIT_WORKTREE="${WEBKIT_WORKTREE:?set WEBKIT_WORKTREE to an isolated build source path}"
REVISION="$(tr -d '[:space:]' < "$PATCH_ROOT/revision")"

git -C "$WEBKIT_BASE" cat-file -e "$REVISION^{commit}"
if [ -e "$WEBKIT_WORKTREE/.git" ]; then
    git -C "$WEBKIT_BASE" worktree remove --force "$WEBKIT_WORKTREE"
elif [ -e "$WEBKIT_WORKTREE" ]; then
    echo "error: refusing to replace non-worktree path: $WEBKIT_WORKTREE" >&2
    exit 1
fi

mkdir -p "$(dirname "$WEBKIT_WORKTREE")"
git -C "$WEBKIT_BASE" worktree add --detach "$WEBKIT_WORKTREE" "$REVISION"

while IFS= read -r patch; do
    case "$patch" in ''|'#'*) continue ;; esac
    patch_path="$PATCH_ROOT/$patch"
    test -s "$patch_path"
    git -C "$WEBKIT_WORKTREE" apply --check "$patch_path"
    git -C "$WEBKIT_WORKTREE" apply "$patch_path"
done < "$PATCH_ROOT/series"

angle_zlib="$WEBKIT_WORKTREE/Source/ThirdParty/ANGLE/third_party/zlib/google"
mkdir -p "$angle_zlib"
cp "$ROOT/wpe-drm/webkit-vendor/angle-zlib/compression_utils_portable.h" "$angle_zlib/"
cp "$ROOT/wpe-drm/webkit-vendor/angle-zlib/compression_utils_portable.cc" "$angle_zlib/"
cp "$ROOT/wpe-drm/ChromeMotionState.h" \
    "$WEBKIT_WORKTREE/Source/WebKit/WPEPlatform/wpe/drm/ChromeMotionState.h"

printf 'WebKit worktree ready: source=%s revision=%s\n' "$WEBKIT_WORKTREE" "$REVISION"
git -C "$WEBKIT_WORKTREE" diff --stat
