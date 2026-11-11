# CLAUDE.md

Native PC port of Tony Hawk's Proving Ground, statically recompiled from the Xbox 360
`default.xex` with the ReXGlue SDK. See README.md, docs/BUILDING.md and docs/DEVELOPMENT.md.

Machine-specific notes (install locations, paths, personal workflow) belong in
`CLAUDE.local.md`, which is git-ignored. Never put them in this file: the repository is public.

## Building

- Windows: `.\build.ps1` (Release build in `out\build\release`, staged in `out\dist`);
  `-Install` also installs it. Linux: `./build.sh`. Per-step logs are in `out\logs`.
- The game executable is never committed (`assets/` is git-ignored); the launcher checks its
  version. Neither are game data or the generated code.
- `build.ps1` clones the SDK with `core.symlinks=false`, then replaces libmspack's symlink
  placeholders in `thirdparty\rexglue-sdk\thirdparty\libmspack\cabextract\mspack` with the real
  files. Without that, `lzxd.c` fails with `expected identifier or '('`.
- If a configure step fails after a toolchain change, delete `out\build\release` (CMake caches
  tool paths) and build again.

## Source layout rules

- `thirdparty\`, `generated\`, `out\`, `bin\` and `assets\` contents are build inputs/outputs,
  not source. Change source under `src\`.
- SDK changes go through `patches\rexglue-sdk.patch` (docs/BUILDING.md, "Updating the SDK
  patch"). The SDK is compiled with `-fno-char8_t`: don't name `std::u8string`.
- When `patches\rexglue-sdk.patch` changes, the build warns; delete `thirdparty\rexglue-sdk`
  and build again so the SDK is fetched and patched fresh.
- An installed build keeps its `pg.toml` (settings), `logs` and `portable.txt`; never delete an
  install folder to "clean" it, or the settings are lost.
