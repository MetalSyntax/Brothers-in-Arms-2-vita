# Brothers in Arms 2: Global Front HD — PS Vita Port

<p align="center">
  <img src="extras/livearea/pic0.png" width="700" alt="Brothers in Arms 2 PS Vita Banner" />
</p>

<p align="center">
  <b>Native port of Brothers in Arms 2: Global Front HD (Gameloft) for PlayStation Vita and PlayStation TV.</b>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Platform-PS%20Vita%20%7C%20PS%20TV-003791.svg?style=flat-square&logo=playstation" alt="Platform PS Vita" />
  <img src="https://img.shields.io/badge/Title%20ID-PSVBIA002-ff69b4.svg?style=flat-square" alt="Title ID PSVBIA002" />
  <img src="https://img.shields.io/badge/Engine-Gameloft%20GAND%202010-brightgreen.svg?style=flat-square" alt="Engine" />
  <img src="https://img.shields.io/badge/Renderer-vitaGL%20%28GLES%201.1%29-orange.svg?style=flat-square" alt="Renderer" />
  <img src="https://img.shields.io/badge/Status-Playable-success.svg?style=flat-square" alt="Status: Playable" />
</p>

---

## 📖 Description

**Brothers in Arms 2: Global Front HD** is Gameloft's acclaimed World War II first-person shooter, originally released for Android (`Brothers_In_Arms_2__Global_Front_HD_1.0.4`, package `com.gameloft.android.GAND.GloftBia2.bia2`). This port runs the original compiled native library (`libbia2.so`, armeabi-v7a) directly on the PS Vita's ARM Cortex-A9 processor, using a dynamic loader (*soloader*) and an Android environment emulation layer (*FalsoJNI*), with [vitaGL](https://github.com/Rinnegatamante/vitaGL) handling hardware-accelerated GLES rendering.

### 🎮 Current Status: Playable

The game boots through the intro video, menus and campaign, and plays smoothly in-game with full 3D rendering and audio.

### ✨ Key Features & Improvements

- **Native ARM Execution**: `libbia2.so` (armeabi-v7a) runs natively on the Vita CPU without interpretation or virtual machine overhead.
- **Accurate Textures (ATC → DXT transcoding)**: The game ships Qualcomm/AMD ATC textures, which the Vita GPU cannot sample. `source/reimpl/atc.c` transcodes every ATC block straight into an S3TC block (ATC RGB → DXT1, explicit alpha → DXT3, interpolated alpha → DXT5) with the correct channel order: no blue skin or blue rifles, and textures stay compressed (4/8 bpp) in VRAM.
- **Full Physical Dual-Analog Controls**: Mapped dual sticks and physical buttons through synthetic touch injection directly into the engine's coordinate space.
- **Bottom Virtual Buttons Dimmed to 1%**: Bottom HUD touch buttons (move stick, fire, aim, run, stance) are dimmed to 1% opacity to keep the screen clean while staying touch-responsive. The top HUD (pause button, weapon/ammo panel, objectives) keeps the game's normal look, and the in-game *Controls opacity* option is left untouched.
- **Intro Video Support**: `intro.mp4` (H.264 Baseline + AAC) plays directly on boot using the Vita's hardware video decoder (`sceAvcdec`) paired with software AAC audio via FFmpeg (`libavcodec`). Skippable at any time with Cross or Start.
- **Unlimited Camera with Adjustable Sensitivity**: Right analog stick provides continuous 360° camera rotation with curved deadzones and 1-10 sensitivity levels (adjustable via **Select + D-Pad Up / Down** with an on-screen indicator bar). Setting is saved to `ux0:data/brothersinarms2/camera_sens.txt`.
- **Run with Circle**: Holding Circle holds the game's own *dash* button (its position is read from the level HUD data), so the soldier sprints without opening any menu.
- **Accelerometer & Shake**: The Vita motion sensor feeds the game's accelerometer input, so "SHAKE" melee prompts can be passed by shaking the console, or by pressing **Select**.
- **Performance**: vitaGL built with the CPU-only speedhacks proven on N.O.V.A 2 (`MATH_SPEEDHACK`, `SAMPLERS_SPEEDHACK`), cached RAM pool, compressed textures, trampoline-based engine hooks (no per-call code patching), ARM 444 MHz / GPU 222 MHz clocks.
- **High Quality Audio**: Custom 48 kHz stereo audio mixer routing sound effects and Vorbis OGG playback to `sceAudioOut`.
- **Save & Storage Redirection**: File paths remapped to `ux0:data/brothersinarms2/`.

---

## 🕹️ Controls

| Vita Input | Gameplay Action | Menu Action |
|---|---|---|
| **Left Analog Stick** | Move soldier | Navigate menu |
| **Right Analog Stick** | Camera look (hold to rotate, light tilt = precise aim) | — |
| **R / RTRIGGER / R1** | Fire weapon (hold) | — |
| **L / LTRIGGER / L1** | Aim / Iron sights (toggle) | — |
| **Cross** | Context Action (Climb, jump obstacles, vehicle interact) | Confirm |
| **Circle** | Run / dash (hold) | Back / Cancel (`AKEYCODE_BACK`) |
| **Square** | Stance change (Stand / Crouch / Prone) | — |
| **Triangle** | Switch weapon / Reload | — |
| **Start** | Pause menu | Open menu (`AKEYCODE_MENU`) |
| **Select** | Shake the console (melee "SHAKE" prompts) | — |
| **Select + D-Pad Up / Down** | Adjust camera sensitivity (1 to 10; bar appears on screen) | — |
| **Shaking the console** | Melee "SHAKE" prompts (real accelerometer) | — |
| **D-Pad (without Select)** | Move soldier (up/down/left/right) | Navigate menu |
| **Touchscreen** | Original touch input (still active, bottom buttons at 1% opacity) | Menu navigation |

---

## 📋 Prerequisites

To run this port on your PS Vita or PS TV, you will need:

1. A PS Vita or PS TV on Custom Firmware (**HENkaku** / **h-encore** / **Enso**), firmware 3.60–3.74.
2. [**kubridge**](https://github.com/TheOfficialFloW/kubridge/releases) and [**FdFix**](https://github.com/TheOfficialFloW/FdFix/releases) installed as kernel plugins in `ur0:tai/config.txt` under `*KERNEL`.
3. [**libshacccg.suprx**](https://github.com/Rinnegatamante/ShaRKBR33D/releases/latest) installed in `ur0:data/` ([extraction guide](https://samilops2.gitbook.io/vita-troubleshooting-guide/shader-compiler/extract-libshacccg.suprx)).
4. A legally obtained copy of **Brothers in Arms 2: Global Front HD v1.0.4** (`.apk`) and its data cache folder (`BIA2/`).

---

## 📦 Installation Instructions

1. Install `brothersinarms2.vpk` on your PS Vita using **VitaShell**.
2. Extract your Android APK (`Brothers_In_Arms_2__Global_Front_HD_1.0.4.apk`) on your PC.
3. Copy the following files to `ux0:data/brothersinarms2/`:
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

---

## 🛠️ Building from Source

### Build Prerequisites

- **VitaSDK** (with softfp ABI support).
- Installed packages: `kubridge`, `pthread`, and vita-portlibs `ffmpeg` (`avcodec`, `avutil`, `swresample`, `mp3lame`).
- [vitaGL](https://github.com/Rinnegatamante/vitaGL) vendored in `lib/vitagl` (built automatically during compilation).
- CMake and Make or Ninja.

### Build Commands

```bash
git submodule update --init --recursive
export VITASDK=/path/to/vitasdk
export PATH=$VITASDK/bin:$PATH
cmake -B build -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

The resulting `brothersinarms2.vpk` and `eboot.bin` will be located in the `build/` directory.

---

## 🏗️ Project Architecture

- `source/main.c`: Game lifecycle bootstrap, JNI initialization, render loop, video initialization.
- `source/input.c` & `source/input.h`: Dual analog sticks, physical button mapping to synthetic touch events, dash button lookup, accelerometer / shake, camera sensitivity OSD.
- `source/patch.c`: Trampoline hooks that dim the bottom HUD controls to 1% (joystick, fire, aim/dash/stance items), screen resolution setup.
- `source/video.cpp` & `source/video_avc.c`: Intro video demuxer and playback using PS Vita hardware H.264 decoding (`sceAvcdec`) and software AAC decoding.
- `source/reimpl/atc.c`: Qualcomm/AMD ATC → S3TC (DXT1/DXT3/DXT5) block transcoder with corrected color channel mapping.
- `source/audio.c`: Audio mixer and sound effect streaming.
- `source/reimpl/`: POSIX and Bionic libc reimplementations, filesystem remapping, pthreads, and memory management.

---

## ⚖️ Disclaimer

**Brothers in Arms** and **Gameloft** are registered trademarks of Gameloft and Ubisoft. This project is an unofficial port and is not endorsed or sanctioned by Gameloft or Ubisoft.

No copyrighted game assets or proprietary binaries are distributed in this repository. Users must supply their own legally purchased copy of the Android game to use this port.

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

## 👥 Credits and Acknowledgements

- **Gameloft & Gearbox Software**: Original developers and publishers of Brothers in Arms 2: Global Front.
- **TheFloW**: Original Android soloader architecture, `kubridge`, and `FdFix`.
- **Rinnegatamante**: `vitaGL` and invaluable contributions to the PS Vita development community.
- **v-atamanenko**: `FalsoJNI` and `soloader-boilerplate`.
- **Wonder Diaz**: Porting, reverse engineering, controls mapping, and optimization for PS Vita.
- **PS Vita Homebrew Community**: Ongoing support and tools.

---

## License

This software is licensed under the MIT License. See [LICENSE](LICENSE) for details.
