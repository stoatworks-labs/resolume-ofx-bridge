# Build a corpus of real OFX plugins to develop and test against, on Windows.
#
# The Windows half of scripts/build-test-plugins.sh. EDIT BOTH: they exist to
# produce the same corpus from the same sources, and a plugin present on one
# platform and not the other is a gap in the evidence rather than a difference
# in the product. They are separate files rather than one because nothing about
# them is shared -- clang/-dynamiclib/.bundle on one side, cl/LD/Contents\Win64
# on the other.
#
# Not built here:
#   metalgain  - Metal is a macOS API.
#   openclgain - the host advertises OpenCL only where the GL interop exists,
#                which on Windows it does not yet. A plugin built against a
#                path the host declines would test nothing.
#
# Output: build\test-plugins\<name>.ofx.bundle\Contents\Win64\<name>.ofx
#
#   powershell -ExecutionPolicy Bypass -File scripts\build-test-plugins.ps1
#
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$ofx  = Join-Path $root 'external\openfx'
$out  = Join-Path $root 'build\test-plugins'
$work = Join-Path $root 'build\ofx-support'

if (-not (Test-Path (Join-Path $ofx 'include\ofxCore.h'))) {
    throw 'external/openfx is missing. Run: git submodule update --init --recursive'
}

# ---------------------------------------------------------------------------
# The compiler
#
# cl.exe needs the environment vcvarsall.bat exports; it will not run usefully
# without it. vswhere ships with any VS 2017+ installer, so it is the supported
# way to find the install rather than guessing at Program Files paths.
# ---------------------------------------------------------------------------
$arch = if ($env:OFX_TEST_ARCH) { $env:OFX_TEST_ARCH } else { 'x64' }

function Import-VcVars([string]$targetArch) {
    if ($env:VCToolsInstallDir -and $env:VSCMD_ARG_TGT_ARCH -eq $targetArch) { return }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw "vswhere not found at $vswhere; is Visual Studio installed?" }

    $install = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if (-not $install) { throw 'no Visual Studio install with the C++ toolset was found' }

    $vcvars = Join-Path $install 'VC\Auxiliary\Build\vcvarsall.bat'
    if (-not (Test-Path $vcvars)) { throw "vcvarsall.bat not found under $install" }

    # Host and target may differ: an ARM64 machine building x64 binaries is a
    # normal way to work here, and vcvarsall spells that "arm64_x64".
    $host_ = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'x64' }
    $spec  = if ($host_ -eq $targetArch) { $targetArch } else { "${host_}_${targetArch}" }

    Write-Host "==> importing the MSVC environment ($spec)"
    cmd /c "`"$vcvars`" $spec >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
    }
    if (-not $env:VCToolsInstallDir) { throw "vcvarsall.bat $spec did not set up a toolchain" }
}

Import-VcVars $arch

# ---------------------------------------------------------------------------
# The OpenFX C++ plugin Support library, which the examples are written against.
#
# Compiled directly rather than through the upstream CMake project, for the
# reason given in the shell script: that project resolves EXPAT through Conan.
# ---------------------------------------------------------------------------
Write-Host "==> building OpenFX Support library ($arch)"
New-Item -ItemType Directory -Force -Path $work | Out-Null

$lib = Join-Path $work 'OfxSupport.lib'
$objs = @()
foreach ($src in Get-ChildItem (Join-Path $ofx 'Support\Library\*.cpp')) {
    $obj = Join-Path $work ($src.BaseName + '.obj')
    & cl /nologo /c /std:c++17 /O2 /EHsc /MD /DWIN32 /D_WINDOWS /DNOMINMAX `
        /I "$ofx\include" /I "$ofx\Support\include" `
        "$($src.FullName)" /Fo"$obj" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "compiling $($src.Name) failed" }
    $objs += $obj
}
& lib /nologo /OUT:"$lib" $objs | Out-Null
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $lib)) { throw 'OfxSupport.lib was not produced' }

# ---------------------------------------------------------------------------
# The examples
#
# Invert       - the minimal filter: no parameters at all
# Basic        - doubles with display ranges, a boolean, a group and a page
# ChoiceParams - choice parameters with named options
# Custom       - General context only; exercises our "cannot host this" path
# OpenGL       - implements OFX OpenGL render
# ---------------------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $out | Out-Null

foreach ($ex in @('Invert', 'Basic', 'ChoiceParams', 'Custom', 'OpenGL')) {
    $src = Get-ChildItem (Join-Path $ofx "Examples\$ex\*.cpp") -EA SilentlyContinue | Select-Object -First 1
    if (-not $src) { Write-Host "  skip $ex (no source)"; continue }

    $name = $src.BaseName
    # Contents\Win64, not Contents\MacOS: an OFX host looks in the directory
    # named for its own architecture, and only a macOS host falls back.
    $bdl = Join-Path $out "$name.ofx.bundle\Contents\Win64"
    New-Item -ItemType Directory -Force -Path $bdl | Out-Null

    $obj = Join-Path $work "$name.obj"
    & cl /nologo /c /std:c++17 /O2 /EHsc /MD /DWIN32 /D_WINDOWS /DNOMINMAX `
        /I "$ofx\include" /I "$ofx\Support\include" /I "$ofx\Examples\include" `
        "$($src.FullName)" /Fo"$obj" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "compiling $ex failed" }

    & link /nologo /DLL /OUT:"$bdl\$name.ofx" $obj "$lib" opengl32.lib user32.lib gdi32.lib | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "linking $ex failed" }

    Write-Host "  built $name.ofx.bundle"
}

Write-Host ''
Write-Host "test plugins in: $out"
Write-Host "try: .\build\Release\ofxprobe.exe --dir $out"
