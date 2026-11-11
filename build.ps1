# Builds Proving Ground Recompiled on Windows.
#
#   .\build.ps1                      Release build in out\build\release, staged in out\dist
#   .\build.ps1 -Configuration RelWithDebInfo   Build with symbols and performance counters
#   .\build.ps1 -Install             Also copy the build to -InstallDir and add a Start menu shortcut
#
# Steps: fetch ReXGlue v0.10.0 into thirdparty\rexglue-sdk and apply patches\rexglue-sdk.patch
# (first run only), build the rexglue code generator, generate code from assets\default.xex,
# then build the game. Requires Visual Studio 2022 (or Build Tools) with the C++ workload and
# its Clang component, or a separate LLVM/Clang passed with -LlvmBin.
[CmdletBinding()]
param(
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug')]
    [string]$Configuration = 'Release',
    [ValidateRange(1, 256)][int]$Jobs = [Environment]::ProcessorCount,
    # An existing, already patched ReXGlue source tree instead of thirdparty\rexglue-sdk.
    [string]$SdkSource = $env:REXSDK_SOURCE,
    # Folder with clang.exe/clang++.exe, if not the one that comes with Visual Studio.
    [string]$LlvmBin = $env:LLVM_BIN,
    # Leave AMD FidelityFX (the FSR 3 upscaler option) out of the renderer. It is built by
    # default, which downloads the FidelityFX SDK.
    [switch]$NoFidelityFX,
    # Leave out the Direct3D 12 renderer (the one with FSR 4); the game then has Vulkan only.
    [switch]$NoD3D12,
    # Only generate the code; do not build the game.
    [switch]$CodegenOnly,
    # Skip copying the game and its libraries to out\dist.
    [switch]$NoStage,
    # After staging, copy the game to -InstallDir (keeping pg.toml, logs and anything else already
    # there) and create or refresh a "Proving Ground Recompiled" Start menu shortcut.
    [switch]$Install,
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'Programs\ProvingGroundRecomp')
)

$ErrorActionPreference = 'Stop'
if ($Install -and ($NoStage -or $CodegenOnly)) { throw '-Install cannot be combined with -NoStage or -CodegenOnly.' }
$FidelityFX = -not $NoFidelityFX
$D3D12 = -not $NoD3D12
$repo = $PSScriptRoot
$sdkTag = 'v0.10.0'
$sdkUrl = 'https://github.com/rexglue/rexglue-sdk.git'
$patch = Join-Path $repo 'patches\rexglue-sdk.patch'

# Optional per-machine settings (ignored by Git): { "sdkSource": "...", "llvmBin": "..." }
$settingsFile = Join-Path $repo '.local\toolchain.json'
if (Test-Path -LiteralPath $settingsFile) {
    $settings = Get-Content -LiteralPath $settingsFile -Raw | ConvertFrom-Json
    if (-not $SdkSource -and $settings.sdkSource) { $SdkSource = $settings.sdkSource }
    if (-not $LlvmBin -and $settings.llvmBin) { $LlvmBin = $settings.llvmBin }
}

if (-not (Test-Path -LiteralPath (Join-Path $repo 'assets\default.xex'))) {
    throw 'Missing assets\default.xex. Extract your Xbox 360 copy of the game into the assets folder first.'
}

$logs = Join-Path $repo 'out\logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null

function Invoke-Step {
    param([string]$Name, [string]$Executable, [string[]]$Arguments, [string]$WorkingDirectory = $repo)
    $log = Join-Path $logs "$Name.log"
    Write-Host "==> $Name" -ForegroundColor Cyan
    $previous = $ErrorActionPreference
    Push-Location $WorkingDirectory
    try {
        # Windows PowerShell treats native stderr as errors; judge by the exit code instead.
        $ErrorActionPreference = 'Continue'
        & $Executable @Arguments 2>&1 | ForEach-Object { "$_" } | Set-Content -LiteralPath $log -Encoding utf8
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
        Pop-Location
    }
    if ($code -ne 0) {
        Get-Content -LiteralPath $log -Tail 40 | Write-Host
        throw "$Name failed (exit $code). Full log: $log"
    }
}

# --- ReXGlue SDK source ---
$git = (Get-Command git -ErrorAction SilentlyContinue).Source
if ($SdkSource) {
    $sdk = (Resolve-Path -LiteralPath $SdkSource).Path
} else {
    $sdk = Join-Path $repo 'thirdparty\rexglue-sdk'
    $marker = Join-Path $sdk '.pg-patch'
    $patchHash = (Get-FileHash -LiteralPath $patch -Algorithm SHA256).Hash
    if (-not (Test-Path -LiteralPath (Join-Path $sdk 'CMakeLists.txt'))) {
        if (-not $git) { throw 'Git is required to fetch the ReXGlue SDK.' }
        Invoke-Step 'sdk-clone' $git @('-c', 'core.symlinks=false', '-c', 'core.longpaths=true', 'clone', '--branch', $sdkTag,
            '--depth', '1', $sdkUrl, $sdk)
        Invoke-Step 'sdk-submodules' $git @('-c', 'core.symlinks=false', '-c', 'core.longpaths=true', 'submodule', 'update',
            '--init', '--recursive', '--depth', '1', '--jobs', '8') $sdk
    }
    if (-not (Test-Path -LiteralPath $marker)) {
        Invoke-Step 'sdk-patch' $git @('apply', '--whitespace=nowarn', $patch) $sdk
        Set-Content -LiteralPath $marker -Value $patchHash -Encoding ascii
    } elseif ((Get-Content -LiteralPath $marker -Raw).Trim() -ne $patchHash) {
        Write-Warning ("patches\rexglue-sdk.patch changed since thirdparty\rexglue-sdk was patched. " +
            'Delete thirdparty\rexglue-sdk to fetch and patch it again.')
    }
}

# The SDK is cloned with core.symlinks=false (symlinks need Developer Mode or admin on Windows), so
# libmspack's symlinked sources in cabextract\mspack, which the SDK compiles from, are checked out as
# one-line text files holding the link target. Replace each with the file it points to.
$mspack = Join-Path $sdk 'thirdparty\libmspack\cabextract\mspack'
if (Test-Path -LiteralPath $mspack) {
    Get-ChildItem -LiteralPath $mspack -File | Where-Object { $_.Length -gt 0 -and $_.Length -lt 256 } | ForEach-Object {
        $link = (Get-Content -LiteralPath $_.FullName -Raw).Trim()
        if ($link -match '^\.\.[\\/][\w.\\/-]+$') {
            $target = [IO.Path]::GetFullPath((Join-Path $mspack $link))
            if (-not (Test-Path -LiteralPath $target -PathType Leaf)) { throw "libmspack link target is missing: $target" }
            Copy-Item -LiteralPath $target -Destination $_.FullName -Force
        }
    }
}

# --- Compiler environment ---
$env:VSLANG = '1033'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio 2022 or its Build Tools (C++ workload) are required.' }
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio installation with the C++ x64 tools was found.' }
$devcmd = Join-Path $vs 'Common7\Tools\VsDevCmd.bat'
$previous = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
cmd /d /c "`"$devcmd`" -arch=amd64 -host_arch=amd64 -no_logo >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}
$ErrorActionPreference = $previous

$cmakeRoot = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$paths = @((Join-Path $cmakeRoot 'CMake\bin'), (Join-Path $cmakeRoot 'Ninja'))
if ($LlvmBin) {
    $paths = @((Resolve-Path -LiteralPath $LlvmBin).Path) + $paths
} else {
    foreach ($candidate in @((Join-Path $env:ProgramFiles 'LLVM\bin'), (Join-Path $vs 'VC\Tools\Llvm\x64\bin'))) {
        if (Test-Path -LiteralPath (Join-Path $candidate 'clang++.exe')) { $paths = @($candidate) + $paths; break }
    }
}
$env:PATH = ($paths -join ';') + ';' + $env:PATH
$clang = (Get-Command clang++ -ErrorAction SilentlyContinue).Source
if (-not $clang) { throw 'Clang was not found. Install the "C++ Clang Compiler for Windows" Visual Studio component or pass -LlvmBin.' }
$clangC = (Get-Command clang -ErrorAction Stop).Source
$cmake = (Get-Command cmake -ErrorAction Stop).Source
$ninja = (Get-Command ninja -ErrorAction Stop).Source
Write-Host "Compiler: $clang"

# --- Configure, build the code generator, generate, build the game ---
$build = Join-Path $repo ("out\build\{0}" -f $Configuration.ToLowerInvariant())
$llvmTools = Split-Path -Parent $clang
$vulkanArgs = @()
if ($FidelityFX -and -not $env:VULKAN_SDK) {
    # FidelityFX only needs the Vulkan headers, which the SDK carries, but its find_package(Vulkan)
    # also wants a loader library: make an import library from the system vulkan-1.dll.
    $vulkanImport = Join-Path $repo 'out\vulkan-import\vulkan-1.lib'
    if (-not (Test-Path -LiteralPath $vulkanImport)) {
        $def = Join-Path $repo 'out\vulkan-import\vulkan-1.def'
        New-Item -ItemType Directory -Force -Path (Split-Path $vulkanImport) | Out-Null
        $names = & (Join-Path $llvmTools 'llvm-readobj.exe') --coff-exports "$env:SystemRoot\System32\vulkan-1.dll" |
            Select-String '^\s*Name: (\S+)' | ForEach-Object { $_.Matches[0].Groups[1].Value }
        Set-Content -LiteralPath $def -Encoding ascii -Value (@('LIBRARY vulkan-1.dll', 'EXPORTS') + $names)
        & (Join-Path $llvmTools 'llvm-dlltool.exe') -m i386:x86-64 -d $def -l $vulkanImport
        if ($LASTEXITCODE -ne 0) { throw 'Could not create the Vulkan import library' }
    }
    $vulkanArgs = @("-DVulkan_INCLUDE_DIR=$sdk\thirdparty\vulkan-headers\include", "-DVulkan_LIBRARY=$vulkanImport")
}
function Configure([string]$Game) {
    $configureArgs = @('-S', $repo, '-B', $build, '-G', 'Ninja',
        "-DCMAKE_BUILD_TYPE=$Configuration", "-DCMAKE_CXX_COMPILER=$clang", "-DCMAKE_C_COMPILER=$clangC",
        "-DCMAKE_MAKE_PROGRAM=$ninja", "-DREXSDK_DIR=$sdk", "-DPG_BUILD_GAME=$Game",
        "-DREXGLUE_USE_D3D12=$(if ($D3D12) { 'ON' } else { 'OFF' })", '-DREXGLUE_USE_VULKAN=ON', '-DREXGLUE_BUILD_TESTS=OFF',
        '-DCMAKE_C_FLAGS=-march=x86-64-v2', '-DCMAKE_CXX_FLAGS=-march=x86-64-v2',
        "-DREXGLUE_ENABLE_FIDELITYFX=$(if ($FidelityFX) { 'ON' } else { 'OFF' })",
        # FidelityFX built from source is for the Vulkan presenter; the D3D12 one loads AMD's
        # runtime DLLs instead (with D3D12 on, "auto" would pick the D3D12 backend).
        '-DREXGLUE_FIDELITYFX_BACKEND=vk') + $vulkanArgs
    Invoke-Step "configure-$Game" $cmake $configureArgs
}

Configure 'OFF'
Invoke-Step 'build-codegen' $cmake @('--build', $build, '--target', 'rexglue', '--parallel', "$Jobs")
# The SDK writes its tools to its own out\<platform> folder, named with a per-configuration suffix.
$toolName = 'rexglue{0}.exe' -f @{ Release = ''; RelWithDebInfo = 'rd'; Debug = 'd' }[$Configuration]
$rexglue = @($build, (Join-Path $sdk 'out')) | Where-Object { Test-Path -LiteralPath $_ } |
    ForEach-Object { Get-ChildItem -LiteralPath $_ -Recurse -Filter $toolName } |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $rexglue) { throw 'The rexglue code generator was not built.' }
# FidelityFX writes its runtime DLL to bin\ in the repository; the tool and the game load it.
$ffxBin = Join-Path $repo 'bin'
if ($FidelityFX) { $env:PATH = "$ffxBin;$env:PATH" }
Invoke-Step 'codegen' $rexglue.FullName @('codegen', (Join-Path $repo 'pg_config.toml'))
if ($CodegenOnly) { Write-Host 'Code generated in generated\.'; exit 0 }

Configure 'ON'
Invoke-Step 'build-game' $cmake @('--build', $build, '--target', 'pg', '--parallel', "$Jobs")
$exe = Join-Path $build 'ProvingGround.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "Build finished but $exe is missing." }
if ($FidelityFX) {
    Get-ChildItem -LiteralPath $ffxBin -Filter 'amd_fidelityfx_*.dll' | Copy-Item -Destination $build
}
if ($FidelityFX -and $D3D12) {
    # FSR 4 comes only as AMD's signed D3D12 binaries of the FSR SDK 2, which the D3D12
    # presenter loads at run time: fetch them and their license once, checked against known hashes.
    $ffxDx12 = Join-Path $repo 'out\fidelityfx-dx12-v2.3.0'
    $ffxDx12Files = @{
        'amd_fidelityfx_loader_dx12.dll'   = 'e2d85aa05a9bd9ed8b38935fdf5199372cca6f74c12015143bb6f945ee1608aa'
        'amd_fidelityfx_upscaler_dx12.dll' = 'd0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46'
        'license.md'                       = '6757f2fec461238da6f84f83536ec195e8c036b1773a06f6f34301b9ed8c0963'
    }
    New-Item -ItemType Directory -Force -Path $ffxDx12 | Out-Null
    foreach ($name in $ffxDx12Files.Keys) {
        $file = Join-Path $ffxDx12 $name
        if (-not (Test-Path -LiteralPath $file)) {
            $url = "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.3.0/Kits/FidelityFX/signedbin/$name"
            if ($name -eq 'license.md') { $url = 'https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.3.0/docs/license.md' }
            Write-Host "Downloading $name (FidelityFX SDK 2.3.0)"
            Invoke-WebRequest -Uri $url -OutFile $file -UseBasicParsing
        }
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash -ne $ffxDx12Files[$name]) {
            Remove-Item -LiteralPath $file
            throw "$name does not match the expected FidelityFX SDK 2.3.0 file"
        }
        if ($name -like '*.dll') { Copy-Item -LiteralPath $file -Destination $build }
    }
}

if (-not $NoStage) {
    $dist = Join-Path $repo 'out\dist\ProvingGroundRecomp'
    if (Test-Path -LiteralPath $dist) { Remove-Item -LiteralPath $dist -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $dist | Out-Null
    Copy-Item -LiteralPath $exe -Destination $dist
    Get-ChildItem -LiteralPath $build -Filter '*.dll' | Copy-Item -Destination $dist
    Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination $dist
    Copy-Item -LiteralPath (Join-Path $repo 'THIRD_PARTY_NOTICES.md') -Destination $dist
    Copy-Item -LiteralPath (Join-Path $repo 'docs\PLAYING.md') -Destination (Join-Path $dist 'README.md')
    # The license texts of everything in the executable and the DLLs (see THIRD_PARTY_NOTICES.md).
    $licenses = Join-Path $dist 'licenses'
    New-Item -ItemType Directory -Force -Path $licenses | Out-Null
    $licenseFiles = [ordered]@{
        'ReXGlue-SDK.txt'             = Join-Path $sdk 'LICENSE'
        'FFmpeg-LGPL-2.1.txt'         = Join-Path $sdk 'thirdparty\FFmpeg\COPYING.LGPLv2.1'
        'FFmpeg.md'                   = Join-Path $sdk 'thirdparty\FFmpeg\LICENSE.md'
        'libmspack-LGPL-2.1.txt'      = Join-Path $sdk 'thirdparty\libmspack\libmspack\COPYING.LIB'
        'SDL3.txt'                    = Join-Path $sdk 'thirdparty\sdl3\LICENSE.txt'
        'DearImGui.txt'               = Join-Path $sdk 'thirdparty\imgui\LICENSE.txt'
        'fmt.txt'                     = Join-Path $sdk 'thirdparty\fmt\LICENSE'
        'spdlog.txt'                  = Join-Path $sdk 'thirdparty\spdlog\LICENSE'
        'tomlplusplus.txt'            = Join-Path $sdk 'thirdparty\tomlplusplus\LICENSE'
        'CLI11.txt'                   = Join-Path $sdk 'thirdparty\cli11\LICENSE'
        'xxHash.txt'                  = Join-Path $sdk 'thirdparty\xxHash\LICENSE'
        'SIMDe.txt'                   = Join-Path $sdk 'thirdparty\simde\COPYING'
        'Vulkan-Headers.md'           = Join-Path $sdk 'thirdparty\vulkan-headers\LICENSE.md'
        'VulkanMemoryAllocator.txt'   = Join-Path $sdk 'thirdparty\vulkan-memory-allocator\LICENSE.txt'
        'glslang.txt'                 = Join-Path $sdk 'thirdparty\glslang\LICENSE.txt'
        'SPIRV-Tools.txt'             = Join-Path $sdk 'thirdparty\spirv-tools\LICENSE'
        'o1heap.txt'                  = Join-Path $sdk 'thirdparty\o1heap\LICENSE'
        'utfcpp.txt'                  = Join-Path $sdk 'thirdparty\utfcpp\LICENSE'
        'aes_128.txt'                 = Join-Path $sdk 'thirdparty\aes_128\LICENSE'
        'Project8Recomp-BSD-3-Clause.txt' = Join-Path $repo 'licenses\Project8Recomp-BSD-3-Clause.txt'
    }
    if ($Configuration -ne 'Release') { $licenseFiles['Tracy.txt'] = Join-Path $sdk 'thirdparty\tracy\LICENSE' }
    if ($FidelityFX) { $licenseFiles['FidelityFX-SDK-1.1.4.txt'] = Join-Path $build '_deps\fidelityfx-src\LICENSE.txt' }
    if ($FidelityFX -and $D3D12) { $licenseFiles['FidelityFX-SDK-2.3.0.md'] = Join-Path $ffxDx12 'license.md' }
    foreach ($name in $licenseFiles.Keys) {
        Copy-Item -LiteralPath $licenseFiles[$name] -Destination (Join-Path $licenses $name)
    }
    Write-Host "Ready to play: $dist\ProvingGround.exe" -ForegroundColor Green
} else {
    Write-Host "Built: $exe" -ForegroundColor Green
}

if ($Install) {
    $installExe = Join-Path $InstallDir 'ProvingGround.exe'
    $running = Get-Process -Name 'ProvingGround' -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and ($_.Path -ieq $installExe) }
    if ($running) { throw "The game is running from $InstallDir. Close it and run the install again." }
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    # Copy over the top: pg.toml, logs and portable.txt are not in the staged build and stay as they are.
    Copy-Item -Path (Join-Path $dist '*') -Destination $InstallDir -Recurse -Force
    $shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'Proving Ground Recompiled.lnk'
    $link = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcut)
    $link.TargetPath = $installExe
    $link.WorkingDirectory = $InstallDir
    $link.IconLocation = "$installExe,0"
    $link.Description = "Tony Hawk's Proving Ground (Recompiled)"
    $link.Save()
    Write-Host "Installed: $installExe (Start menu: Proving Ground Recompiled)" -ForegroundColor Green
}
