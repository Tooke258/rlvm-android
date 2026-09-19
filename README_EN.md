# RLVM for Android

An Android port of [RLVM](http://rlvm.org/), the free software reimplementation
of the RealLive interpreter, packaged as a modern Android application.

This is the English translation of [README.md](README.md).

## What this is, and what it is not

- **It is** an Android port layer for RLVM. The upstream engine (`libreallive`,
  `machine`, `modules`, `systems/base`) is kept as-is; this repository provides
  the platform backends it needs (graphics, audio, text, events, file system)
  plus a minimal Android shell.
- **It is not** a game. It contains no game data, voice packs, images or scripts.
  It only reads a game directory you legally own, through Android's Storage
  Access Framework (SAF).

## Supported games

The table below is copied from upstream RLVM's README and reflects its desktop
(Linux/macOS) verification. This port reuses that engine, so the compatibility is
expected to be the same, but **only Kud Wafter has been verified on device so
far** (title screen through the prologue, voice, BGM, save/load and config).

| Japanese Edition Games | Status | English Fan Patch Status |
| ---------------------- | ------ | ------------------------ |
| Kanon Standard Edition | OK     | NDT's patch              |
| Air Standard Edition   | OK     | (None)                   |
| CLANNAD                | OK     | (Not supported)          |
| CLANNAD (Full Voice)   | OK     | Licensed                 |
| Planetarian CD         | OK     | Licensed                 |
| Tomoyo After           | OK     | (None)                   |
| Little Busters         | OK     | (Untested)               |
| Kud Wafter             | OK     | (None)                   |

| US Edition Games | Status |
| ---------------- | ------ |
| Planetarian      | Works  |

Other titles **may** work; upstream's implementation is reasonably complete, but
the table only lists what has actually been tested, and this port has not gone
through them one by one. Voice playback supports KOE / NWK / OVK archives as well
as Ogg Vorbis voice patches that follow the
`<packnumber>/z<packnumber><sampleid>.ogg` convention.

When reporting a problem, please include the `rlvm-stdout` / `rlvm-stderr` log
lines: upstream sends unimplemented opcodes and swallowed instruction exceptions
to those streams, and this port wires them to logcat.

## License (important)

RLVM is released under the **GNU GPLv3 (or later)**, and so is this port: see
`LICENSE` (the full GPLv3 text) and `COPYING.TXT` (upstream's summary of its own
and its dependencies' licenses).

Because this repository modifies some upstream files, the GPLv3 requires the
complete corresponding source to be available - that is this repository itself.
The upstream source tree is kept in place (`rlvm-release-0.14/`) so the changes
can be reviewed:

```bash
git diff b87ff44 HEAD -- rlvm-release-0.14
```

Distributed binaries (APKs) are covered by the GPLv3 as well: **if you ship an
APK, you must ship this source too.**

## Building

Prerequisites:

| Item | Version / path |
| --- | --- |
| JDK | 17 or newer (AGP 9 bundles Kotlin; no separate Kotlin plugin is used) |
| Android SDK | compileSdk 36, build-tools 36.0.0 |
| Android NDK | 28.2.13676358 (r28c) |
| Boost | 1.92.0 **source tree** (required; see below) |
| CMake | 3.22.1 (the SDK-provided one is fine) |

Third-party dependencies (FreeType / libogg / libvorbis) are not committed; fetch
them with:

```powershell
powershell -File tools/setup_third_party.ps1
```

Boost must be a source tree (the local default is `E:\boost-1.92.0`); set an
environment variable or pass it to Gradle:

```powershell
$env:BOOST_SOURCE_DIR = "D:\boost-1.92.0"
```

Build:

```powershell
.\gradlew.bat assembleDebug     # debug (also includes x86_64 for emulators)
.\gradlew.bat assembleRelease   # release (arm64-v8a and armeabi-v7a only)
```

Release signing: put your keystore in the repository root and create
`keystore.properties`:

```properties
storeFile=your-release.jks
storePassword=***
keyAlias=***
keyPassword=***
```

Both are covered by `.gitignore`. **If the file is absent the release variant
falls back to the debug signing config**, so a fresh clone builds, installs and
runs without any signing material.

## Usage

1. Copy your game directory (containing `Gameexe.ini`, `Seen.txt`, ...) to the
   device;
2. Tap "pick game directory" and grant that directory in the system picker (the
   grant is persisted);
3. Tap "run SAF engine". The screen shows only the game plus a floating ball on
   the right edge; tap the ball to open the control panel and the log.

Touch controls (the equivalents of a mouse):

| Gesture | Acts as | Purpose |
| --- | --- | --- |
| Tap | **Left button** | Advance dialogue, pick menu items |
| **Long press** | **Right button** | Open the in-game menu (save / load / config / quit) |
| Drag | Mouse movement | Move the cursor (hover highlighting, button hit tests) |
| Floating ball | - | Show/hide the control panel and log; draggable, snaps to an edge |

> RealLive opens the in-game menu on a right click. Android has no right button,
> so a long press (400ms threshold) stands in for it. That is also how you quit a
> game normally - otherwise the only way out is killing the process.

Saves and settings live in the app's external files directory, under
`.rlvm/<REGNAME>/` (**not** inside the game directory - the PC releases of
RealLive use a native save format that is not interchangeable with RLVM's):

```
/sdcard/Android/data/org.rlvm.android/files/.rlvm/KEY_<game>/
    global.sav.gz     # global settings (config)
    save000.sav.gz    # save slot
```

## Platform backends

| Module | Implementation |
| --- | --- |
| Graphics | `AndroidSurface`: CPU pixel surface, presented through GLES3; four fit modes (fit / fill width / fill height / stretch) |
| Audio | AAudio (available since API 26, well below our minSdk 31, so no Oboe): decode thread -> lock-free ring buffer -> audio callback |
| Text | FreeType with the system CJK font (no commercial font is bundled) |
| Input | Touch mapped to game-frame coordinates; long press acts as the right mouse button |
| Files | Everything goes through the `GameFileSystem` abstraction: a POSIX backend and a SAF backend, for both reading and writing |

Further details and the pitfalls encountered are in `docs/`: `PROGRESS.md` (the
handoff document), `ARCHITECTURE.md` (upstream source analysis), `DECISIONS.md`
(key trade-offs) and `TESTING.md` (the on-device test procedure).

## Diagnostics

On-device iteration does not need a rebuild: rename `tools/rlvm-diag.sample.txt`
to `rlvm-diag.txt`, push it into the app's external files directory, and it
controls the observation parameters (per-instruction tracing, graphics-stack
dump, run duration, audio statistics, ...). Log tags: `rlvm-native`,
`rlvm-audio`, `rlvm-gl`, `rlvm-font`, `rlvm-graphics`, plus upstream's own
diagnostic output on `rlvm-stdout` / `rlvm-stderr`.
