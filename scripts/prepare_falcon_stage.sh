#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)

# aiot-cli recursively follows XKB symlinks while cleaning an existing stage.
# Recreating the generated stage avoids ENOENT failures on consecutive builds.
rm -rf "$ROOT/.falcon_" "$ROOT/.falcon_tmp"
