# CLAUDE.md

Native PC port of Tony Hawk's Proving Ground, statically recompiled from the Xbox 360
`default.xex` with the ReXGlue SDK (see README.md, docs/BUILDING.md).

## The owner's setup (Windows)

| What | Where |
| --- | --- |
| Repository (this fork) | `C:\src\ProvingGroundRecomp`, remote `origin`. Keep it at this short path; some SDK files have long names |
| Game executable for codegen | `assets\default.xex`, SHA-1 `ea1848544ad48e9b59e089bc0066a6b271e126ba` (git-ignored, never commit it) |
| Installed game (the copy that gets played) | `%LOCALAPPDATA%\Programs\ProvingGroundRecomp` |
| Start menu shortcut | `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Proving Ground Recompiled.lnk` |
| Extracted game data the launcher points at | A folder on the owner's Desktop. It is not used by the build; leave it alone |

Toolchain: Visual Studio 2022 Build Tools (C++ workload, provides CMake and Ninja), LLVM/Clang
in `C:\Program Files\LLVM` (Clang 20+ required), Git, and Python 3 in
`%LOCALAPPDATA%\Programs\Python\Python313` (glslang needs it at configure time).

## Updating the installed build

When the owner asks to update, rebuild or reinstall, do this:

1. Make sure the game is closed (the install step refuses to overwrite a running exe).
2. `git -C C:\src\ProvingGroundRecomp pull --ff-only` (if it is not a fast-forward, stop and ask).
3. Build and install in one step, from the repo root, with the user PATH ahead of the machine
   PATH so the real Python wins over the Microsoft Store `python.exe` stub:

   ```powershell
   $env:Path = [Environment]::GetEnvironmentVariable('Path','User') + ';' + [Environment]::GetEnvironmentVariable('Path','Machine')
   powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Install
   ```

   This is a long build (10-30 minutes on a fresh tree, faster when incremental); run it in the
   background. Per-step logs are in `out\logs`.
4. `-Install` copies `out\dist\ProvingGroundRecomp` over the install folder and refreshes the
   Start menu shortcut. It keeps `pg.toml` (settings), `logs` and `portable.txt`; saves live in
   `Documents\ProvingGroundRecomp` and are never touched. Never delete the install folder to
   "clean" it, or the settings are lost.

If `patches\rexglue-sdk.patch` changed in the pull, the build warns; delete
`thirdparty\rexglue-sdk` and run the build again so the SDK is fetched and patched fresh.
If a configure step fails after a toolchain change, delete `out\build\release` (CMake caches
tool paths) and rebuild.

## Build notes

- `build.ps1` clones the SDK with `core.symlinks=false`, then replaces libmspack's symlink
  placeholders in `thirdparty\rexglue-sdk\thirdparty\libmspack\cabextract\mspack` with the real
  files. Without that, `lzxd.c` fails with `expected identifier or '('`.
- `thirdparty\`, `generated\`, `out\`, `bin\` and `assets\` contents are build inputs/outputs,
  not source. Change source under `src\`, SDK changes go through `patches\rexglue-sdk.patch`
  (see docs/BUILDING.md, "Updating the SDK patch").
