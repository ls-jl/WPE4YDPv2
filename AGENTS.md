# Repository Guidelines

## Project Structure & Module Organization

This repository packages a HaasUI MiniApp shell plus an embedded Direct WPE DRM browser runtime.

- `src/`: MiniApp UI and lifecycle code. `src/pages/index` is the launcher; `src/pages/frame` hosts the fullscreen `<hole>` and starts WPE. `src/utils/keyboard.js` wraps system keyboard calls.
- `jsapi/`: native MiniApp JSAPI bridge and support modules.
- `wpe-drm/`: WPE DRM launcher, testable browser modules, supervisor modules, and the production WebKit patch series.
- `assets/wpe-runtime/`: packaged runtime executed from the installed MiniApp bundle.
- `libs/arm64-orange/`: built native JSAPI library used by packaging.
- `tools/`: device/API notes and architecture documentation.
- `.falcon_/`, `.cache/`, `*.amr`, logs, and archives are generated artifacts unless explicitly tracked.

## Build, Test, and Development Commands

- `npm run sync`: copies generated native/runtime assets into the Falcon build staging area.
- `npm test`: runs JS fixtures, native logic tests, SQLite tests, shell checks, runtime integrity checks, and patch-series validation.
- `npm run build`: syncs assets and produces `8001779591038449.1_0_0.amr`.
- `npm run build:dev`: builds without packaging for faster local iteration.
- `npm run check`: runs `aiot-cli check`.
- `npm run build:simulator`: starts the MiniApp simulator build flow.
- Device install example:
  `adb push 8001779591038449.1_0_0.amr /userdisk/ && adb shell 'miniapp_cli install /userdisk/8001779591038449.1_0_0.amr'`.

## Coding Style & Naming Conventions

Use two-space indentation in Vue/JS files and keep methods small and explicit. Native C/C++ uses the existing brace style and `snake_case` helpers for C code, `camelCase` or existing project style for C++ methods. Prefer ASCII unless editing existing localized UI strings or documentation. Do not add fallback paths to external `/userdisk` runtimes; package required runtime files under `assets/wpe-runtime/`.

## Testing Guidelines

Run `npm test`, `npm run check`, and build the AMR before device testing. Native pure-logic tests live in `wpe-drm/tests`; JavaScript fixtures live under `scripts/test_*.mjs`. For device validation, check MiniApp logs in `/userdata/applog/YD_PEN_APP.log`, WPE logs under the app data `browser/wpe-drm.log`, and verify WPE/WebProcess lifecycle with `pidof wpe-drm-minimal WPEWebProcess WPENetworkProcess`.

## Commit & Pull Request Guidelines

Commit history uses short imperative summaries, for example `Add textarea-backed keyboard request handling`. Keep commits scoped to one behavior or subsystem. PRs should include a concise problem statement, implementation summary, build result, device test notes, and screenshots or logs when UI, DRM, keyboard, or runtime behavior changes.

## Security & Configuration Tips

Do not commit credentials, device-private logs, or unreviewed binary blobs. Keep certificate, font, Mesa, WPE, and GStreamer dependencies inside `assets/wpe-runtime/` when they are required at runtime.
