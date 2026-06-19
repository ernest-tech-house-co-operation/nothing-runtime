# scripts/build.ps1 — Windows PowerShell build script.
#
# Uses the MSVC toolchain (cl.exe) that ships with Visual Studio Build
# Tools. Requires the vendored JSC prebuilt at third_party/bun-webkit/
# (run scripts/fetch_jsc.ps1 first).
#
# Usage:
#   .\scripts\build.ps1                     # builds to build\nth.exe
#   .\scripts\build.ps1 -OutPath out\nth.exe
#
# Prerequisites:
#   - Visual Studio 2019+ with C++ workload (cl.exe + Windows SDK)
#   - Run from a "Developer PowerShell for VS" window, OR this script
#     will auto-detect and load the VS environment via vswhere.
#
# Note: the JSC prebuilt for Windows is built with MSVC, so we MUST use
# MSVC here (not MinGW) — mismatching C++ ABIs would fail at link time.

[CmdletBinding()]
param(
    [string]$OutPath = "build\nth.exe"
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $Root

$OutDir = Split-Path -Parent $OutPath
if ($OutDir -and !(Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }

# --- Locate cl.exe via vswhere if not already in PATH ----------------------
if (!(Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (!(Test-Path $vswhere)) {
        $vswhere = "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
    }
    if (!(Test-Path $vswhere)) {
        throw "cl.exe not on PATH and vswhere not found. Run from a 'Developer PowerShell for VS'."
    }
    $vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsRoot) { throw "No Visual Studio with C++ tools found." }
    $vcvars = Join-Path $vsRoot "Common7\Tools\VsDevCmd.bat"
    if (!(Test-Path $vcvars)) { throw "VsDevCmd.bat not found at $vcvars" }
    # Load the VS environment by capturing its env vars.
    cmd /c "`"$vcvars`" -arch=x64 -host_arch=x64 >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
        }
    }
    if (!(Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw "Failed to load MSVC environment. Run from a 'Developer PowerShell for VS'."
    }
}

# --- Discover JSC ----------------------------------------------------------
$WebKit = Join-Path $Root "third_party\bun-webkit"
$jscHeader = Join-Path $WebKit "include\JavaScriptCore\JavaScript.h"
if (!(Test-Path $jscHeader)) {
    throw "JSC prebuilt not found at $jscHeader. Run: .\scripts\fetch_jsc.ps1"
}

# Find the static libs — Windows prebuilt may use .lib naming.
$libDir = Join-Path $WebKit "lib"
$jscLibs = @(
    "JavaScriptCore.lib", "WTF.lib", "bmalloc.lib",
    "icui18n.lib", "icuuc.lib", "icutu.lib", "icudata.lib"
)
$libArgs = @()
foreach ($lib in $jscLibs) {
    $p = Join-Path $libDir $lib
    if (Test-Path $p) { $libArgs += $p }
}
if ($libArgs.Count -eq 0) {
    # Try lib*.a naming in case it's actually a MinGW build (rare).
    $libArgs = Get-ChildItem -Path $libDir -Filter "*.lib" | ForEach-Object { $_.FullName }
    if ($libArgs.Count -eq 0) {
        throw "No JSC .lib files found in $libDir"
    }
}

# --- Sources ---------------------------------------------------------------
$srcs = @(
    "src\main.cpp",
    "src\cli\parser.cpp",
    "src\config\config.cpp",
    "src\chain\executor.cpp",
    "src\js\engine.cpp",
    "src\js\module_loader.cpp",
    "src\js\globals.cpp",
    "src\js\fetch.cpp",
    "src\js\http_server.cpp",
    "src\server\http.cpp",
    "src\server\websocket.cpp",
    "src\runtimes\role_resolver.cpp",
    "src\runtimes\pm2.cpp",
    "src\util\subprocess.cpp",
    "src\util\net.cpp",
    "src\util\sha1.cpp"
)

$incArgs = @(
    "-Isrc",
    "-Ithird_party",
    "-I`"$libDir\..\include`""
)

$cxxflags = @(
    "/std:c++17",
    "/O2",
    "/EHsc",
    "/utf-8",
    "/W3",
    "/wd4267", "/wd4244", "/wd4146", "/wd4018",  # size-conversion warnings common in JSC headers
    "/DNOMINMAX",
    "/DWIN32_LEAN_AND_MEAN",
    "/D_CRT_SECURE_NO_WARNINGS"
)

$linkArgs = @(
    "ws2_32.lib",
    "bcrypt.lib",
    "advapi32.lib"
) + $libArgs

Write-Host "Compiling with cl.exe..." -ForegroundColor Cyan
$args = @("/nologo", "/Fe:$OutPath") + $cxxflags + $incArgs + $srcs + "/link" + $linkArgs
& cl.exe @args
if ($LASTEXITCODE -ne 0) { throw "cl.exe failed with exit code $LASTEXITCODE" }

Write-Host "Built: $OutPath" -ForegroundColor Green
