# Assemble the on-device test fixture for the RLVM Android port.
#
# Base data comes from RLVM's own upstream test fixtures (GPLv3 project, no
# commercial game assets are used anywhere in this repository). The upstream
# Gameexe.ini under test/Gameexe_data/ is deliberately minimal, so running the
# *full* engine path (RLMachine + AddAllModules + AddGameHacks) needs a handful
# of extra keys that every real game's Gameexe.ini contains. Those are appended
# here rather than editing the upstream file, so `git diff rlvm-release-0.14/`
# stays a faithful record of our source changes.
#
# Output goes to build/probe-fixture/ (gitignored). Push it with adb; see
# docs/TESTING.md.
#
# NOTE: keep this file ASCII-only. Windows PowerShell 5.1 reads .ps1 files
# without a BOM using the ANSI code page, which corrupts non-ASCII characters.

[CmdletBinding()]
param(
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'

$scriptPath = $MyInvocation.MyCommand.Path
if ($scriptPath) {
    $repoRoot = Split-Path -Parent (Split-Path -Parent $scriptPath)
} else {
    $repoRoot = (Get-Location).Path
}
if (-not $OutDir) { $OutDir = Join-Path $repoRoot 'build\probe-fixture' }

$upstreamTest = Join-Path $repoRoot 'rlvm-release-0.14\test'
$gameexeSrc = Join-Path $upstreamTest 'Gameexe_data\Gameexe.ini'
$seenSrc = Join-Path $upstreamTest 'Module_Mem_SEEN\cpyrng_0.TXT'

foreach ($p in $gameexeSrc, $seenSrc) {
    if (-not (Test-Path $p)) { throw "Missing upstream test fixture: $p" }
}

$OutDir = (New-Item -ItemType Directory -Force -Path $OutDir).FullName

Copy-Item -LiteralPath $gameexeSrc -Destination (Join-Path $OutDir 'Gameexe.ini') -Force
Copy-Item -LiteralPath $seenSrc -Destination (Join-Path $OutDir 'Seen.txt') -Force

# Keys read without a default during engine assembly. A real game always has
# them; the upstream minimal fixture does not.
#
# Values must be non-empty strings: GameexeInterpretObject::ToString() throws
# "Unknown Gameexe key" when the value vector has no string at index 0, so
# "#KEY=" (empty) is NOT enough.
$extraKeys = @(
    '#DISKMARK="none"'    # read unconditionally by AddGameHacks(); "none" matches no hack
)

Add-Content -LiteralPath (Join-Path $OutDir 'Gameexe.ini') -Value $extraKeys -Encoding utf8

Write-Host "Probe fixture assembled at $OutDir"
Get-ChildItem $OutDir | ForEach-Object { "  {0,-14} {1} bytes" -f $_.Name, $_.Length }
