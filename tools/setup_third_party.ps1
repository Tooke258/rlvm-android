# Fetch third-party source dependencies for the RLVM Android port.
#
# Why this is needed: the RLVM core (149 translation units) does NOT depend on SDL,
# but it depends heavily on Boost -- above all boost::serialization, which carries
# the save-game format and cannot be replaced by the standard library.
#
# Boost is extracted into third_party/ and compiled from source by CMake, which
# bypasses Boost's own b2 build system.
#
# Usage:
#   powershell -File tools/setup_third_party.ps1
#   powershell -File tools/setup_third_party.ps1 -BoostVersion 1.86.0
#
# Manual download (recommended when the official archive is slow):
#   Put boost-1.86.0.tar.gz (or boost_1_86_0.tar.gz / .tar.xz, any of them will do)
#   into third_party/ and re-run this script. A local archive is used before any
#   network access is attempted.
#   GitHub: https://github.com/boostorg/boost/releases/download/boost-1.86.0/boost-1.86.0.tar.gz
#
# NOTE: keep this file ASCII-only. Windows PowerShell 5.1 reads .ps1 files without
#       a BOM using the ANSI code page, which corrupts non-ASCII characters.

[CmdletBinding()]
param(
    [string]$BoostVersion = '1.86.0',
    [string]$DestRoot,
    [string[]]$Url
)

$ErrorActionPreference = 'Stop'

# $PSScriptRoot is not populated when default parameter values are evaluated on
# Windows PowerShell 5.1, so resolve the script directory here instead.
$scriptPath = $MyInvocation.MyCommand.Path
if ($scriptPath) {
    $repoRoot = Split-Path -Parent (Split-Path -Parent $scriptPath)
} else {
    $repoRoot = (Get-Location).Path
}
if (-not $DestRoot) { $DestRoot = Join-Path $repoRoot 'third_party' }

$DestRoot = (New-Item -ItemType Directory -Force -Path $DestRoot).FullName
$underscored = 'boost_' + ($BoostVersion -replace '\.', '_')

function Find-BoostRoot {
    Get-ChildItem -LiteralPath $DestRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName 'boost\version.hpp') } |
        Select-Object -First 1 -ExpandProperty FullName
}

$existing = Find-BoostRoot
if ($existing) {
    Write-Host "Boost $BoostVersion already present at $existing"
    exit 0
}

# ---------------------------------------------------------------------------
# 1) Prefer an archive the user already downloaded into third_party/.
# ---------------------------------------------------------------------------
$localArchive = Get-ChildItem -LiteralPath $DestRoot -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^boost[-_].*\.tar\.(gz|xz)$' } |
    Sort-Object Length -Descending |
    Select-Object -First 1

$sources = @()
if ($localArchive) { $sources += $localArchive.FullName }

$candidates = if ($Url) { $Url } else { @(
    "https://github.com/boostorg/boost/releases/download/boost-$BoostVersion/boost-$BoostVersion.tar.gz",
    "https://archives.boost.io/release/$BoostVersion/source/$underscored.tar.gz"
) }
$sources += $candidates

$curl = Join-Path $env:SystemRoot 'System32\curl.exe'
if (-not (Test-Path $curl)) { $curl = 'curl.exe' }

# Native commands writing to stderr become terminating errors under
# ErrorActionPreference=Stop on Windows PowerShell 5.1. Relax it around curl/tar and
# judge success purely from $LASTEXITCODE.
$savedEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'

$extracted = $false
foreach ($source in $sources) {
    $isLocal = Test-Path -LiteralPath $source -PathType Leaf

    if ($isLocal) {
        $archivePath = $source
        Write-Host "Using local archive $archivePath"
    } else {
        $suffix = if ($source.EndsWith('.tar.xz')) { '.tar.xz' } else { '.tar.gz' }
        $archivePath = Join-Path $DestRoot "$underscored$suffix"
        Write-Host "Downloading $source"
        & $curl --fail --location --silent --show-error --retry 2 --output $archivePath --max-time 1800 $source 2>&1 | Out-Null
        $curlExit = $LASTEXITCODE
        if ($curlExit -ne 0 -or -not (Test-Path $archivePath)) {
            Write-Host "  download failed (curl exit $curlExit)"
            if (Test-Path $archivePath) { Remove-Item -LiteralPath $archivePath -Force }
            continue
        }
        $sizeMb = [math]::Round((Get-Item $archivePath).Length / 1MB, 1)
        Write-Host "  downloaded $sizeMb MB"
    }

    Write-Host '  extracting...'
    & tar.exe -xf $archivePath -C $DestRoot 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  tar failed (exit $LASTEXITCODE)"
        continue
    }

    if ($isLocal) {
        Write-Host '  (keeping the local archive)'
    } else {
        Remove-Item -LiteralPath $archivePath -Force
    }
    $extracted = $true
    break
}

$ErrorActionPreference = $savedEap

if (-not $extracted) {
    throw "Could not obtain Boost $BoostVersion. Tried: $($sources -join ', ')"
}

$boostRoot = Find-BoostRoot
if (-not $boostRoot) {
    throw "Extraction succeeded but no 'boost/version.hpp' found under $DestRoot"
}

Write-Host "Boost $BoostVersion ready at $boostRoot"
