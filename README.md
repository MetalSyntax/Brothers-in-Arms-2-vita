<h1 align="center">
  <img src="extras/livearea/pic0.png" alt="Brothers in Arms 2 PS Vita Banner" width="700"><br>
  Brothers in Arms 2: Global Front HD · PS Vita Port
</h1>

<p align="center">
  <b>Native port of Brothers in Arms 2: Global Front HD (Gameloft) for PlayStation Vita and PlayStation TV.</b>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Platform-PS%20Vita%20%7C%20PS%20TV-003791.svg?style=flat-square&logo=playstation" alt="Platform" />
  <img src="https://img.shields.io/badge/Origin-Android-3DDC84.svg?style=flat-square&logo=android" alt="Origin" />
  <img src="https://img.shields.io/badge/Title%20ID-PSVBIA002-ff69b4.svg?style=flat-square" alt="Title ID" />
  <img src="https://img.shields.io/badge/Engine-Gameloft%20GAND%202010-brightgreen.svg?style=flat-square" alt="Engine" />
  <img src="https://img.shields.io/badge/Renderer-vitaGL%20%28GLES%201.1%29-orange.svg?style=flat-square" alt="Renderer" />
  <img src="https://img.shields.io/badge/Status-In--game-yellow.svg?style=flat-square" alt="Status" />
</p>

<p align="center">
  <a href="#-description">Description</a> •
  <a href="#-features">Features</a> •
  <a href="#-controls">Controls</a> •
  <a href="#-installation">Installation</a> •
  <a href="#-known-issues">Known Issues</a> •
  <a href="#-building-from-source">Build</a> •
  <a href="#-credits">Credits</a>
</p>

---

## 📖 Description

**Brothers in Arms 2: Global Front HD** is Gameloft's World War II first-person shooter, originally released for Android
(`Brothers_In_Arms_2__Global_Front_HD_1.0.4`, package `com.gameloft.android.GAND.GloftBia2.bia2`). This port runs the
original compiled native library (`libbia2.so`, armeabi-v7a) directly on the PS Vita's ARM Cortex-A9 processor, using a
dynamic loader (*soloader*) and an Android environment emulation layer (*FalsoJNI*), with
[vitaGL](https://github.com/Rinnegatamante/vitaGL) handling the GLES rendering.

---

## 🎮 Status & Playability

**Status: In-game (v1.0)**. On real hardware the game boots through the intro video and menus, and reaches gameplay
with 3D rendering. The notes record in-mission frame rates of about 15–25 FPS before the last optimizations; those
changes have not been measured yet, and a full playthrough has not been recorded.

- [x] **Boot & intro**: `intro.mp4` plays on boot via the hardware decoder (221 frames in log 008). *Verified on console.*
- [x] **Graphics**: ATC textures are transcoded to S3TC (DXT1/DXT3/DXT5) and the blue-skin / blue-rifle bug is fixed;
  3D rendering runs on the console (GPU hang fixed in the title-screen bug). *Verified on console.*
- [x] **Menus**: the main-menu background is restored. *Verified on console.*
- [x] **Dash and accelerometer**: the dash button rectangle is read from the HUD data and the accelerometer produces live samples (log 008). *Verified on console.*
- [ ] **Camera, SHAKE and HUD opacity**: fixed in logs 007 and 008; the axis check for the glider and the bottom-HUD dimming are not confirmed.
- [ ] **Audio**: the sound pool and music were observed in logs; the 48 kHz mixer has not been checked end to end.
- [ ] **Performance**: mission FPS of about 15–25 (log 006 saw 40–60); the post-log-008 optimizations are not measured yet (`brothersinarms2_009.log` pending).

### Key Features & Improvements

- **Native ARM execution**: `libbia2.so` (armeabi-v7a) runs natively on the Vita CPU without interpretation or a virtual machine.
- **Accurate textures (ATC → DXT transcoding)**: the game ships Qualcomm/AMD ATC textures, which the Vita GPU cannot
  sample. `source/reimpl/atc.c` transcodes every ATC block straight into an S3TC block (ATC RGB → DXT1, explicit alpha →
  DXT3, interpolated alpha → DXT5) with the correct channel order: no blue skin or blue rifles, and textures stay
  compressed (4/8 bpp) in VRAM.
- **Full physical dual-analog controls**: the sticks and physical buttons are mapped through synthetic touch injection
  into the engine's coordinate space.
- **Bottom virtual buttons dimmed to 1%**: the bottom HUD touch buttons (move stick, fire, aim, run, stance) are dimmed to
  1% opacity to keep the screen clean while staying touch-responsive. The top HUD (pause button, weapon/ammo panel,
  objectives) keeps the game's normal look, and the in-game *Controls opacity* option is left untouched.
- **Intro video**: `intro.mp4` (H.264 Baseline + AAC) plays on boot using the Vita's hardware video decoder (`sceAvcdec`)
  with software AAC audio via FFmpeg (`libavcodec`). Skippable at any time with Cross or Start.
- **Unlimited camera with adjustable sensitivity**: the right analog stick gives continuous 360° camera rotation with
  curved deadzones and 1–10 sensitivity levels (adjust with **Select + D-Pad Up / Down**, with an on-screen indicator bar).
  The setting is saved to `ux0:data/brothersinarms2/camera_sens.txt`.
- **Run with Circle**: holding Circle holds the game's own *dash* button (its position is read from the level HUD data), so
  the soldier sprints without opening any menu.
- **Accelerometer & shake**: the Vita motion sensor feeds the game's accelerometer input, so "SHAKE" melee prompts can be
  passed by shaking the console, or by pressing **Select**.
- **Performance**: vitaGL built with the CPU-only speedhacks proven on N.O.V.A 2 (`MATH_SPEEDHACK`, `SAMPLERS_SPEEDHACK`),
  cached RAM pool, compressed textures, trampoline-based engine hooks (no per-call code patching), ARM 444 MHz / GPU 222 MHz clocks.
- **High-quality audio**: custom 48 kHz stereo audio mixer routing sound effects and Vorbis OGG playback to `sceAudioOut`.
- **Save & storage redirection**: file paths remapped to `ux0:data/brothersinarms2/`.

### Changelog

**v1.0 — Initial Release** (`RELEASE.md`, `CMakeLists.txt` `VITA_VERSION` 01.00)
- **Full 3D gameplay** on the PS Vita with hardware-accelerated GLES rendering via vitaGL.
- **Blue textures fixed**: ATC textures are transcoded block-by-block to DXT1 / DXT3 / DXT5 (`source/reimpl/atc.c`) with the
  proper channel order, fixing the blue skin and blue rifle textures while keeping textures compressed in VRAM.
- **Performance**: vitaGL CPU speedhacks from N.O.V.A 2 (`MATH_SPEEDHACK`, `SAMPLERS_SPEEDHACK`), cached vitaGL RAM pool,
  compressed textures and trampoline hooks for the HUD dimming (no code patching on every HUD draw).
- **Full dual-analog physical controls**: left stick / D-Pad move, right stick gives a smooth, unlimited 360° camera with
  continuous drag and velocity curves, R / L fire and aim, Cross context action, Circle run / dash (Back in menus; it no
  longer opens the pause menu), Select shake, Square stance, Triangle weapon switch / reload, Start pause.
- **Bottom virtual buttons dimmed to 1%** while remaining touch-interactive; the top HUD keeps its normal opacity.
- **Main-menu background restored**: a hook that made the menu background render black was fixed.
- **Smooth camera sensitivity**: adjust with **Select + D-Pad Up / Down** (levels 1–10), with an on-screen indicator;
  settings are saved to `ux0:data/brothersinarms2/camera_sens.txt`.
- **Intro video playback** with hardware H.264 decoding and software AAC, skippable with Cross or Start.
- **Audio system**: custom 48 kHz stereo mixer routing sound effects and Vorbis OGG music to `sceAudioOut`.
- **Data paths**: all save and asset directories remapped to `ux0:data/brothersinarms2/`.

---

## ✨ Features

| Feature | Status | Notes |
| :--- | :---: | :--- |
| Boots to gameplay | ✅ | Intro video and menus reached on hardware (logs 007–008); gameplay in missions confirmed in log 008 |
| Graphics / renderer | ✅ | vitaGL GLES 1.1 path with ATC → DXT transcoding; 3D rendering confirmed on the console |
| Audio & music | 🧪 | 48 kHz mixer and Vorbis OGG playback; sound pool and music seen in logs, end-to-end check pending |
| Video / cutscenes | ✅ | `intro.mp4` plays with `sceAvcdec` + FFmpeg AAC (221 frames in log 008). Other videos not documented |
| Physical buttons | 🟡 | Dash and accelerometer confirmed in log 008; the other button fixes from log 007 are not confirmed yet |
| Analog sticks / camera | 🟡 | Camera rewrite (no camera limit) fixed in log 007; not confirmed on the console yet |
| Touch screen | 🧪 | Touch input still active; bottom buttons dimmed to 1%. Not verified after the HUD fix |
| Button remapping / port menu | ➖ | No remapping. Camera sensitivity is adjusted in-game with Select + D-Pad Up / Down (saved to `camera_sens.txt`) |
| Save data | 🧪 | Saves redirected to `ux0:data/brothersinarms2/saves/`; not verified across sessions yet |
| Native trophies | ➖ | No trophy data in the repository |
| Languages | 🧪 | Not tested yet |
| Performance | 🟡 | Mission FPS about 15–25 (log 006: 40–60); the newer optimizations are not measured yet |
| Online / IAP / ads | 🧪 | Not tested yet. Facebook and IAP JNI entry points exist in `source/java.c` |
| PS TV | 🧪 | Not tested yet |

Legend: `✅ Working (verified on console) · 🧪 Implemented, not yet verified on console · 🟡 Partial · ❌ Not working · ➖ Not applicable · ❔ Not documented yet`

---

## 🎯 Controls

| Vita input | Gameplay action | Menu action |
| :--- | :--- | :--- |
| **Left Analog Stick** | Move soldier | Navigate menu |
| **Right Analog Stick** | Camera look (hold to rotate, light tilt = precise aim) | — |
| **R / RTRIGGER / R1** | Fire weapon (hold) | — |
| **L / LTRIGGER / L1** | Aim / iron sights (toggle) | — |
| **Cross** | Context action (climb, jump obstacles, vehicle interact) | Confirm |
| **Circle** | Run / dash (hold) | Back / Cancel (`AKEYCODE_BACK`) |
| **Square** | Stance change (stand / crouch / prone) | — |
| **Triangle** | Switch weapon / reload | — |
| **Start** | Pause menu | Open menu (`AKEYCODE_MENU`) |
| **Select** | Shake the console (melee "SHAKE" prompts) | — |
| **Select + D-Pad Up / Down** | Adjust camera sensitivity (1 to 10; bar appears on screen) | — |
| **Shaking the console** | Melee "SHAKE" prompts (real accelerometer) | — |
| **D-Pad (without Select)** | Move soldier (up / down / left / right) | Navigate menu |
| **Touchscreen** | Original touch input (still active, bottom buttons at 1% opacity) | Menu navigation |

---

## 📋 Prerequisites

To run this port on your PS Vita or PS TV, you will need:

1. A PS Vita or PS TV on Custom Firmware (**HENkaku** / **h-encore** / **Enso**), firmware 3.60–3.74.
2. [**kubridge**](https://github.com/TheOfficialFloW/kubridge/releases) and [**FdFix**](https://github.com/TheOfficialFloW/FdFix/releases) installed as kernel plugins in `ur0:tai/config.txt` under `*KERNEL`.
3. [**libshacccg.suprx**](https://github.com/Rinnegatamante/ShaRKBR33D/releases/latest) installed in `ur0:data/` ([extraction guide](https://samilops2.gitbook.io/vita-troubleshooting-guide/shader-compiler/extract-libshacccg.suprx)).
4. A legally obtained copy of **Brothers in Arms 2: Global Front HD v1.0.4** (`.apk`) and its data cache folder (`BIA2/`).

---

## 📦 Installation

1. Install `brothersinarms2.vpk` on your PS Vita using **VitaShell**.
2. Extract your Android APK (`Brothers_In_Arms_2__Global_Front_HD_1.0.4.apk`) on your PC with any zip tool.
3. Copy the following to `ux0:data/brothersinarms2/`:
   - `lib/armeabi-v7a/libbia2.so` from inside the APK → `ux0:data/brothersinarms2/libbia2.so`
   - `res/raw/*` from inside the APK → `ux0:data/brothersinarms2/res/raw/`
   - The entire Android data cache folder `BIA2/` → `ux0:data/brothersinarms2/BIA2/`
     (must contain `intro.mp4`, `sprites/`, `sounds/`, `gui/`, etc.)
4. Launch **Brothers in Arms 2** from your PS Vita LiveArea.

### Final File Layout in `ux0:data/brothersinarms2/`

```text
ux0:data/brothersinarms2/
├── libbia2.so            <- Extracted from lib/armeabi-v7a/ of the APK
├── BIA2/                 <- Android cache folder (gameloft/games/bia2)
│   ├── intro.mp4         <- Boot intro video
│   ├── sprites/
│   ├── sounds/
│   ├── fonts/
│   ├── gui/
│   └── 3d/
├── res/
│   └── raw/              <- Extracted from res/raw/ of the APK (raw_000.ogg, etc.)
├── saves/                <- Created automatically by the game
├── logs/                 <- Debug logs (brothersinarms2_NNN.log)
└── camera_sens.txt       <- Created automatically when camera sensitivity is adjusted
```

### Troubleshooting

- **Error dialog on boot saying data files are missing**: make sure `ux0:data/brothersinarms2/libbia2.so` is present and that
  `ux0:data/brothersinarms2/BIA2/` contains the cache files.
- **Crash on startup or black screen**: check that `kubridge.skprx` and `fd_fix.skprx` are loaded under `*KERNEL` in
  `ur0:tai/config.txt`, and that `ur0:data/libshacccg.suprx` is present.
- **Any other issue**: check the latest log in `ux0:data/brothersinarms2/logs/brothersinarms2_NNN.log`.

---

## 🐛 Known Issues

- **In-mission frame rate**: about 15–25 FPS in the notes (40–60 seen in one log). The newest optimizations are not measured yet.
- **Not played start to finish**: the port has been checked on boot, menus, the intro and in-mission logs, not a full campaign.
- **Audio** and the **touch-screen HUD dimming** still need a check on the console.
- **Glider axes (X/Y)** from the accelerometer are not verified on the console; the first sample is logged.
- **Crash dumps and logs**: the debug logs are in `ux0:data/brothersinarms2/logs/`. Attach the newest log when you open an issue.

---

## 🔨 Building from Source

### Build Prerequisites

- **VitaSDK** (with softfp ABI support).
- Installed packages: `kubridge`, `pthread`, and vita-portlibs `ffmpeg` (`avcodec`, `avutil`, `swresample`, `mp3lame`).
- [vitaGL](https://github.com/Rinnegatamante/vitaGL) vendored in `lib/vitagl` (built automatically during compilation).
- CMake and Make or Ninja.

### Build Commands

```bash
git clone https://github.com/MetalSyntax/Brothers-in-Arms-2-vita.git
cd Brothers-in-Arms-2-vita
git submodule update --init --recursive
export VITASDK=/path/to/vitasdk
export PATH=$VITASDK/bin:$PATH
cmake -B build -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

The resulting `brothersinarms2.vpk` and `eboot.bin` will be located in the `build/` directory.

---

## 📁 Project Structure

- `source/main.c`: game lifecycle bootstrap, JNI initialization, render loop, video initialization.
- `source/input.c` & `source/input.h`: dual analog sticks, physical button mapping to synthetic touch events, dash button
  lookup, accelerometer / shake, camera sensitivity OSD.
- `source/patch.c`: trampoline hooks that dim the bottom HUD controls to 1% (joystick, fire, aim/dash/stance items), screen resolution setup.
- `source/video.cpp` & `source/video_avc.c`: intro video demuxer and playback using PS Vita hardware H.264 decoding (`sceAvcdec`) and software AAC decoding.
- `source/reimpl/atc.c`: Qualcomm/AMD ATC → S3TC (DXT1/DXT3/DXT5) block transcoder with corrected color channel mapping.
- `source/audio.c`: audio mixer and sound effect streaming.
- `source/reimpl/`: POSIX and Bionic libc reimplementations, filesystem remapping, pthreads, and memory management.
- `lib/`: auxiliary libraries (`so_util`, `falso_jni`, `kubridge`, `libc_bridge`, `fios`, `sha1`, `stb`, `vitagl`).
- `PORTING_PLAN.md`: porting plan. `port_progress.md`: bug-by-bug log (bugs 1–8 and the release notes). `RELEASE.md`: release notes.

---

## 📜 Disclaimer

**Brothers in Arms** and **Gameloft** are registered trademarks of Gameloft and Ubisoft. This project is an unofficial port
and is not endorsed or sanctioned by Gameloft or Ubisoft.

No copyrighted game assets or proprietary binaries are distributed in this repository. Users must supply their own legally
purchased copy of the Android game to use this port.

---

## ☕ Support My Work

This project is **free** and will stay free. If you enjoy it, a tip from **$1** helps me keep porting games to the PS Vita:

<p align="center">
  <a href="https://ko-fi.com/metalsyntax"><img src="https://ko-fi.com/img/githubbutton_sm.svg" alt="Support me on Ko-fi"></a>
</p>

- **Ko-fi:** [ko-fi.com/metalsyntax](https://ko-fi.com/metalsyntax)
- **PayPal:** [paypal.me/MetalSyntax](https://paypal.me/MetalSyntax)
- **Bitcoin (BTC):** `133HdCsaWxdjTfmwpgGVykByhNBV338uRc`

**Ko-fi supporters get:**
- **Early access** to new ports before their public release (test builds of upcoming projects).
- **Data preparation scripts and step-by-step guides** to set up your own game files for each new port.
- A say in which games I port next.

> Game files are never distributed. You always need your own legally obtained copy of the game.

---

## 👥 Credits

- **Gameloft & Gearbox Software**: original developers and publishers of Brothers in Arms 2: Global Front.
- **TheFloW**: original Android soloader architecture, `kubridge`, `so_util` and `FdFix`.
- **Rinnegatamante**: `vitaGL`, `ShaRKBR33D` and invaluable contributions to the PS Vita development community.
- **v-atamanenko**: `FalsoJNI` and `soloader-boilerplate`.
- **Wonder Diaz**: porting, reverse engineering, controls mapping, and optimization for PS Vita.
- **PS Vita homebrew community**: ongoing support and tools.

---

## 📄 License

This software is licensed under the MIT License. See [LICENSE](LICENSE) for the full text
(Copyright (C) 2021 Andy Nguyen, Copyright (C) 2021-2022 Rinnegatamante, Copyright (C) 2022-2024 Volodymyr Atamanenko).
