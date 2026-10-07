# RLVM for Android

An Android port of [RLVM](http://rlvm.org/), the free software reimplementation
of the RealLive interpreter, packaged as a modern Android application.

This is the English translation of [README.md](README.md).

## Version / releases

**Latest: `v0.3.0`** (`versionCode 7`) —
<https://github.com/Tooke258/rlvm-android/releases>

Compared to v0.2.4 this release ships both the backlog from the save/gallery/scene-select
line and a set of **real fixes for the Little Busters! EX baseball minigame**:

- **Hit direction is no longer systematically mirrored**: the x87 pop-arith group (`DE`)
  wrote every `FADDP/FMULP/FSUBRP/FSUBP/FDIVRP/FDIVP st(i),st0` destination into `st(1)`;
  for `i≠1` that is "right value, wrong slot". Now bit-identical to the original
  `PT00.dll` under the differential harness.
- **Outfield throw-back no longer lands behind the batter**: the DLL's CRT static
  constructors were never run (a real host runs them via `LoadLibrary`).
- **The UI no longer renders correctly for one instant and then reverts to the default
  image**: LBEX stores its gallery page table at `intA[7200+n]`, while our memory bank
  only had 2000 entries *and* an access limit hard-coded to 2000.
- **Menus no longer leave residue**; icons are no longer overwritten by the engine's
  button-state pattern table; **gallery scene replay no longer hangs** (root cause was
  our own audio layer) and **scene-select choices work**.

Full notes: [`docs/RELEASE-NOTES-v0.3.0.md`](docs/RELEASE-NOTES-v0.3.0.md)
(Chinese).

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

### Verified on device (Android, Redmi K40 Gaming / Android 12)

| Title | Status |
| --- | --- |
| Kud Wafter (Japanese) | Verified: title → text, voice, BGM, save/load, Config |
| Little Busters! EX (**Chinese-patched**) | Main flow usable: dialogue, save/load, gallery (incl. scene replay), scene-select choices; the minigame can be entered and played (hit direction / throw-back landing fixed) |

### Known issues

* Minigame **entity rendering** is still incomplete (cats / ball / batters sometimes
  missing or misplaced), and the practice-select menu (`PT_PR_*`) has gaps.
* Event plates (`PT_ANN*`, `PT_CALL*`) can get stuck on screen in some states; the panel
  has an "Event plates: mute/restore" switch to clear them manually.
* `logic_hz` and `lb_minigame` still live in `rlvm-diag.txt` (see *Diagnostics*):
  **deleting that file** drops the minigame logic rate from the calibrated 37/s back to
  65.7/s.
* Upstream opcodes `Sys 457/2402/2502/1520/1521/366/801` are still unimplemented.

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

Boost must be provided as a source tree. Point the build at it with an
environment variable, `-DBOOST_SOURCE_DIR=`, or a `boost.dir=` entry in the
gitignored `local.properties`:

```powershell
$env:BOOST_SOURCE_DIR = "<path-to-boost>"
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

### Switching the text container (language)

Translated data usually lives in a **separate file** (not the `SEEN.TXT` that ships in the
game directory). The app never asks you to overwrite the original file; it gives you an
explicit switch instead:

1. Put the container file into the game directory (e.g. `Seen-CN.TXT`);
2. Tap "选择文本容器 / Pick text container" in the panel and choose it - the candidate list
   is built by the app from the game directory, **without** the system file picker (on some
   devices picking a large file crashes the system file UI);
3. With the switch **on** the engine reads that container; with it **off** it reads
   `Seen.txt` from the game directory (the original).

A container must declare its own text encoding per scene (the RLdev metadata `encoding`
byte: `0` = CP932, `1` = CP936/GBK), otherwise non-Japanese text is decoded as CP932 and
comes out as mojibake. `Seen####.txt` scene override files in the game directory still take
precedence over the container (that is the patch mechanism's existing semantics).

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

The **normal configuration** is
[`tools/rlvm-diag.default.txt`](tools/rlvm-diag.default.txt):

```ini
lb_minigame=1     # disable upstream's "skip the whole LB baseball minigame" hack
logic_hz=37       # logic-frame pacing, calibrated against PC's measured 37.5 fps
                  # (throttles the script only; rendering is untouched)
```

**Both still live in the diag layer** — deleting/clearing `rlvm-diag.txt` silently
reverts to the defaults (a faster logic rate, and the LB baseball minigame gets skipped).
Keep the file around for stable long-term behaviour. All other instrumentation switches
(`patno_trace`, `gallery_probe`, `wipe_log`, `op_trace`, ...) default to off; enable
them ad hoc.
