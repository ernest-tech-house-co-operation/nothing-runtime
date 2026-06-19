# scripts/fetch_jsc.ps1 — Windows PowerShell equivalent of fetch_jsc.sh
#
# Downloads and extracts the prebuilt JavaScriptCore (and supporting libs)
# from oven-sh/WebKit — the same source Bun itself uses for its JSC embed.
#
# Pinned tag (recorded in BUILD.md):
#   autobuild-cd821fecca0d39c8bac874c283d956868c7f0de0
#
# Usage:
#   .\scripts\fetch_jsc.ps1                 # autodetects windows-amd64
#   .\scripts\fetch_jsc.ps1 windows-amd64
#
# Re-running is safe: if third_party\bun-webkit\include\JavaScriptCore\JavaScript.h
# already exists, the download is skipped (this is the bug avoided from
# Bun's own CMake — see BUILD.md "Known issues avoided").

[CmdletBinding()]
param(
    [string]$Target = ""
)

$ErrorActionPreference = "Stop"

$Tag = if ($env:NTH_WEBKIT_TAG) { $env:NTH_WEBKIT_TAG } else { "autobuild-cd821fecca0d39c8bac874c283d956868c7f0de0" }

if (-not $Target) {
    $Target = "windows-amd64"
}

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$Dest = Join-Path $Root "third_party\bun-webkit"
$Marker = Join-Path $Dest "include\JavaScriptCore\JavaScript.h"

# Idempotency check.
if (Test-Path $Marker) {
    Write-Host "[fetch_jsc] already present at $Dest — skipping." -ForegroundColor Green
    exit 0
}

$Url = "https://github.com/oven-sh/WebKit/releases/download/$Tag/bun-webkit-$Target.tar.gz"
Write-Host "[fetch_jsc] downloading $Url" -ForegroundColor Cyan

$tmpTar = [System.IO.Path]::GetTempFileName() + ".tar.gz"
try {
    # GitHub releases redirect, so use -AllowAutoRedirect (default) + a
    # long timeout since the file is ~300-500 MB.
    Invoke-WebRequest -Uri $Url -OutFile $tmpTar -UseBasicParsing -TimeoutSec 600
} catch {
    throw "Download failed: $_"
}

Write-Host "[fetch_jsc] extracting to $Dest" -ForegroundColor Cyan
if (Test-Path $Dest) { Remove-Item -Recurse -Force $Dest }
New-Item -ItemType Directory -Path $Dest -Force | Out-Null

# Use tar.exe (ships with Windows 10+ since 2018). Use --exclude to skip
# the large Source tree and test binaries that we don't need.
& tar.exe xzf $tmpTar -C (Join-Path $Root "third_party") `
    --exclude='bun-webkit/Source' `
    --exclude='bun-webkit/bin/TestWebKitAPI'

if ($LASTEXITCODE -ne 0) {
    throw "tar.exe failed with exit code $LASTEXITCODE"
}

Remove-Item $tmpTar -ErrorAction SilentlyContinue

if (-not (Test-Path $Marker)) {
    throw "Extraction finished but marker file $Marker missing"
}

Write-Host "[fetch_jsc] done. JSC available at $Dest" -ForegroundColor Green
Write-Host "[fetch_jsc] tag: $Tag"
