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

# Also stage a SEEN0002.TXT override file. This exercises the fan-translation /
# patch mechanism: Archive::ReadOverrides() (path backend) and
# Archive::ApplyOverrides() (SAF backend) must both pick it up, which shows up as
# an extra scenario index in the probe report.
#
# Content matters: a real SEEN####.TXT holds the *scenario payload* itself, not a
# whole SEEN.TXT. The packed file starts with a table of (offset, length) pairs;
# an entry whose offset is 0 does not exist. We therefore slice scenario 1's bytes
# out of Seen.txt and register them as override index 2 - so the fixture is built
# purely from upstream test data and stays free of commercial content.
$seenBytes = [System.IO.File]::ReadAllBytes($seenSrc)
$sliceOffset = -1
$sliceLength = -1
for ($i = 0; $i -lt 16; $i++) {
    $entryOffset = [BitConverter]::ToInt32($seenBytes, $i * 8)
    if ($entryOffset -ne 0) {
        $sliceOffset = $entryOffset
        $sliceLength = [BitConverter]::ToInt32($seenBytes, $i * 8 + 4)
        break
    }
}
if ($sliceOffset -lt 0) { throw 'Could not find a scenario entry in the TOC of Seen.txt' }

$scenarioBytes = New-Object byte[] $sliceLength
[Array]::Copy($seenBytes, $sliceOffset, $scenarioBytes, 0, $sliceLength)
[System.IO.File]::WriteAllBytes((Join-Path $OutDir 'SEEN0002.TXT'), $scenarioBytes)

# Keys read without a default during engine assembly. A real game always has
# them; the upstream minimal fixture does not.
#
# Values must be non-empty strings: GameexeInterpretObject::ToString() throws
# "Unknown Gameexe key" when the value vector has no string at index 0, so
# "#KEY=" (empty) is NOT enough.
$extraKeys = @(
    '#DISKMARK="none"'    # read unconditionally by AddGameHacks(); "none" matches no hack
    '#FOLDNAME.000="g00"' # System::BuildFileSystemCache only scans subdirs listed here
)

Add-Content -LiteralPath (Join-Path $OutDir 'Gameexe.ini') -Value $extraKeys -Encoding utf8

# Fixture for the game-file lookup layer. Taken from upstream test data
# test/Gameroot/g00/doesntmatter.g00 so no commercial content is involved.
$gameRootSrc = Join-Path $upstreamTest 'Gameroot\g00\doesntmatter.g00'
if (Test-Path $gameRootSrc) {
    $g00Dir = Join-Path $OutDir 'g00'
    New-Item -ItemType Directory -Force -Path $g00Dir | Out-Null
    Copy-Item -LiteralPath $gameRootSrc -Destination (Join-Path $g00Dir 'doesntmatter.g00') -Force
} else {
    Write-Host "NOTE: $gameRootSrc not found, skipping g00 lookup fixture"
}

Write-Host "Probe fixture assembled at $OutDir"
Get-ChildItem $OutDir | ForEach-Object { "  {0,-14} {1} bytes" -f $_.Name, $_.Length }

# ---------------------------------------------------------------------------
# Image-loading fixture.
#
# GRPCONV::AssignConverter dispatches by *content*, not by extension, so a plain
# 24-bit Windows BMP placed under a name whose extension is in IMAGE_FILETYPES
# (g00 / pdt) exercises the whole pipeline: FindFile -> OpenFd -> AssignConverter
# -> BMPCONV -> Surface. Generated here so the repository carries no binary asset.
# ---------------------------------------------------------------------------
$bmpWidth = 320
$bmpHeight = 240
$rowBytes = $bmpWidth * 3          # 960, already 4-byte aligned
$pixelBytes = $rowBytes * $bmpHeight
$fileSize = 54 + $pixelBytes       # 14-byte file header + 40-byte DIB header

$stream = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter($stream)
$writer.Write([byte][char]'B')
$writer.Write([byte][char]'M')
$writer.Write([uint32]$fileSize)
$writer.Write([uint32]0)
$writer.Write([uint32]54)          # BMPCONV requires bfOffBits == 0x36
$writer.Write([uint32]40)          # BMPCONV requires biSize == 0x28
$writer.Write([int32]$bmpWidth)
$writer.Write([int32]$bmpHeight)
$writer.Write([uint16]1)           # planes
$writer.Write([uint16]24)          # bits per pixel
$writer.Write([uint32]0)           # BI_RGB
$writer.Write([uint32]$pixelBytes)
$writer.Write([uint32]2835)
$writer.Write([uint32]2835)
$writer.Write([uint32]0)
$writer.Write([uint32]0)

# Rows are stored bottom-up, pixels are BGR.
for ($y = $bmpHeight - 1; $y -ge 0; $y--) {
    for ($x = 0; $x -lt $bmpWidth; $x++) {
        $on = ((([int]($x / 32)) + ([int]($y / 32))) % 2) -eq 0
        if ($on) {
            $writer.Write([byte]32); $writer.Write([byte]32); $writer.Write([byte]224)
        } else {
            $writer.Write([byte]224); $writer.Write([byte]128); $writer.Write([byte]32)
        }
    }
}
$writer.Flush()

$g00Dir = Join-Path $OutDir 'g00'
New-Item -ItemType Directory -Force -Path $g00Dir | Out-Null
[System.IO.File]::WriteAllBytes((Join-Path $g00Dir 'test.g00'), $stream.ToArray())
$writer.Close()

Write-Host "Image fixture: g00\test.g00 ($fileSize bytes, 320x240 BMP content)"

# ---------------------------------------------------------------------------
# Audio fixture: a 3-second 440 Hz sine, 44.1 kHz stereo 16-bit WAV.
#
# Deliberately not 48 kHz so that WAVFILE::MakeConverter has to resample, which
# also exercises the SDL audio shim (compat/sdl_shim). The tone is audible, so a
# human can confirm the pipeline really produces sound.
# ---------------------------------------------------------------------------
$sampleRate = 44100
$seconds = 3
$totalFrames = $sampleRate * $seconds
$dataBytes = $totalFrames * 2 * 2          # stereo, 16-bit

$audioStream = New-Object System.IO.MemoryStream
$audioWriter = New-Object System.IO.BinaryWriter($audioStream)
$audioWriter.Write([char[]]'RIFF')
$audioWriter.Write([uint32](36 + $dataBytes))
$audioWriter.Write([char[]]'WAVE')
$audioWriter.Write([char[]]'fmt ')
$audioWriter.Write([uint32]16)
$audioWriter.Write([uint16]1)              # PCM
$audioWriter.Write([uint16]2)              # stereo
$audioWriter.Write([uint32]$sampleRate)
$audioWriter.Write([uint32]($sampleRate * 4))
$audioWriter.Write([uint16]4)              # block align
$audioWriter.Write([uint16]16)             # bits per sample
$audioWriter.Write([char[]]'data')
$audioWriter.Write([uint32]$dataBytes)

for ($i = 0; $i -lt $totalFrames; $i++) {
    $value = [int16](20000 * [Math]::Sin(2 * [Math]::PI * 440 * $i / $sampleRate))
    $audioWriter.Write($value)
    $audioWriter.Write($value)
}
$audioWriter.Flush()
[System.IO.File]::WriteAllBytes((Join-Path $OutDir 'test.wav'), $audioStream.ToArray())
$audioWriter.Close()

Write-Host "Audio fixture: test.wav ($($dataBytes + 44) bytes, 3s 440Hz stereo)"
