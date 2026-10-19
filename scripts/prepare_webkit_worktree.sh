#!/usr/bin/env bash
set -euo pipefail

ROOT="${PROJECT_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
PATCH_ROOT="$ROOT/wpe-drm/webkit-patches"
WEBKIT_BASE="${WEBKIT_BASE:?set WEBKIT_BASE to the clean WebKit git repository}"
WEBKIT_WORKTREE="${WEBKIT_WORKTREE:?set WEBKIT_WORKTREE to an isolated build source path}"
REVISION="$(tr -d '[:space:]' < "$PATCH_ROOT/revision")"

git -C "$WEBKIT_BASE" cat-file -e "$REVISION^{commit}"
if [ -e "$WEBKIT_WORKTREE/.git" ]; then
    if [ -f "$WEBKIT_WORKTREE/.wpe-replay.json" ] && \
        node "$ROOT/scripts/webkit_replay_state.mjs" check "$ROOT" "$WEBKIT_WORKTREE"; then
        echo "Reusing verified patch replay: $WEBKIT_WORKTREE"
        exit 0
    fi
    echo "error: preserving existing worktree: $WEBKIT_WORKTREE" >&2
    echo 'Choose a fresh WEBKIT_WORKTREE; existing worktrees are never force-removed.' >&2
    exit 1
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
    printf 'Applying WebKit patch: %s\n' "$patch"
    if ! git -C "$WEBKIT_WORKTREE" apply --check "$patch_path"; then
        printf 'error: WebKit patch does not apply to revision %s: %s\n' \
            "$REVISION" "$patch" >&2
        exit 1
    fi
    git -C "$WEBKIT_WORKTREE" apply "$patch_path"
done < "$PATCH_ROOT/series"

node "$ROOT/scripts/apply_webkit_overlays.mjs" "$ROOT" "$WEBKIT_WORKTREE"

node "$ROOT/scripts/webkit_replay_state.mjs" write "$ROOT" "$WEBKIT_WORKTREE"

printf 'WebKit worktree ready: source=%s revision=%s\n' "$WEBKIT_WORKTREE" "$REVISION"
git -C "$WEBKIT_WORKTREE" diff --stat
