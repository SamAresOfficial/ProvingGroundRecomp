# Building

The build generates C++ from your copy of the game, so you need the game files first:
extract your Xbox 360 disc into `assets/` so that `assets/default.xex` exists. Only
`default.xex` is read at build time; at runtime the game asks for the full game folder.

The scripts do everything else:

1. Clone [ReXGlue](https://github.com/rexglue/rexglue-sdk) v0.10.0 into `thirdparty/rexglue-sdk`
   and apply `patches/rexglue-sdk.patch` (first run only).
2. Build the `rexglue` code generator and run `rexglue codegen pg_config.toml`, which writes
   the recompiled code into `generated/`.
3. Build the game and copy it, with its libraries, to `out/dist/ProvingGroundRecomp`.

Logs for each step are in `out/logs`. A full build compiles a lot of generated code; expect
roughly 10 to 30 minutes on a modern 8-core CPU.

## Windows

Requirements:

- Visual Studio 2022 or newer, or its Build Tools, with the **Desktop development with C++**
  workload (it provides CMake, Ninja, the Windows SDK and the linker)
- Clang 20 or newer: the Visual Studio component **C++ Clang Compiler for Windows**, or
  [LLVM](https://github.com/llvm/llvm-project/releases) installed in `C:\Program Files\LLVM`
- Git

```powershell
.\build.ps1                                # Release
.\build.ps1 -Configuration RelWithDebInfo  # symbols, frame counters, Tracy
.\build.ps1 -LlvmBin C:\path\to\llvm\bin   # a specific Clang
.\build.ps1 -Install                       # also install to %LOCALAPPDATA%\Programs\ProvingGroundRecomp
                                           # and add a Start menu shortcut (-InstallDir to change)
```

`-Install` copies over an existing install and keeps its `pg.toml` and `logs`.

Keep the repository in a short path (for example `C:\src\ProvingGroundRecomp`): some SDK
files have long names. Other options: `-Jobs`, `-CodegenOnly`, `-NoStage`, `-SdkSource` (use
an existing patched SDK tree), `-NoFidelityFX` (leave out AMD FidelityFX, which the FSR options
need; Linux builds never include it), `-NoD3D12` (leave out the Direct3D 12 renderer and FSR 4).
Windows builds download AMD's signed FSR SDK 2.3.0 Direct3D 12 runtime
(`amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll`) and check their hashes.

## Linux

Requirements (Ubuntu/Debian names):

```bash
sudo apt install git cmake ninja-build pkg-config clang-20 lld-20 libvulkan-dev \
    libgtk-3-dev libx11-xcb-dev libxss-dev libwayland-dev libwayland-bin wayland-protocols \
    libxkbcommon-dev libdecor-0-dev libasound2-dev libpulse-dev libpipewire-0.3-dev
```

CMake 3.25 or newer is required. Then:

```bash
./build.sh                  # Release
./build.sh RelWithDebInfo   # symbols, frame counters, Tracy
```

`CC`/`CXX` choose the compiler, `JOBS` the parallel jobs and `REXSDK_SOURCE` an existing
patched SDK tree.

## Updating the SDK patch

The SDK changes live in a working tree of ReXGlue v0.10.0. After changing it, export the
patch again from that tree, including new files:

```bash
git add -N <new files> && git diff --binary HEAD > ../ProvingGroundRecomp/patches/rexglue-sdk.patch
```

The build scripts note the patch hash in `thirdparty/rexglue-sdk/.pg-patch` and warn when
the patch changes; delete `thirdparty/rexglue-sdk` to fetch and patch it again.

## IDEs

`CMakePresets.json` has `release`, `relwithdebinfo` and `debug` presets. Run a build script
once first, so the SDK is fetched and the code is generated, then open the folder in Visual
Studio, VS Code or CLion.
