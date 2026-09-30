# Brothers in Arms 2: Global Front HD Vita v1.0

This is a wrapper/port of **Brothers in Arms 2: Global Front HD** (Gameloft) for the PlayStation Vita and PlayStation TV.

## Changelog

- **v1.0 — Initial Release**:
  - **Full 3D Gameplay**: Runs smoothly on the PS Vita using hardware-accelerated GLES rendering via [vitaGL](https://github.com/Rinnegatamante/vitaGL).
  - **Blue Textures Fixed**: ATC textures are transcoded block-by-block to DXT1 / DXT3 / DXT5 (`source/reimpl/atc.c`) with the proper channel order, fixing the blue skin and blue rifle textures while keeping textures compressed in VRAM.
  - **Performance**: vitaGL CPU speedhacks from N.O.V.A 2 (`MATH_SPEEDHACK`, `SAMPLERS_SPEEDHACK`), cached vitaGL RAM pool, compressed textures and trampoline hooks for the HUD dimming (no code patching on every HUD draw).
  - **Full Dual-Analog Physical Controls**:
    - **Left Stick / D-Pad**: Move soldier.
    - **Right Stick**: Smooth, unlimited 360° camera with continuous drag and velocity curves.
    - **R / RTRIGGER / R1**: Fire weapon (hold).
    - **L / LTRIGGER / L1**: Iron sights / Aim (toggle).
    - **Cross**: Context action (climb obstacles, jump, vehicle interaction).
    - **Circle**: Run / dash (hold in game; Back in menus -- no longer opens the pause menu).
    - **Select**: Shake (melee "SHAKE" prompts); the real accelerometer works too.
    - **Square**: Change stance (Stand / Crouch / Prone).
    - **Triangle**: Switch weapon / Reload.
    - **Start**: Pause game / Open menu.
  - **Bottom Virtual Buttons Dimmed to 1%**: Bottom on-screen buttons (move joystick, fire, aim, run, stance) are rendered at 1% opacity while remaining touch-interactive; the top HUD (pause button, weapon panel) keeps its normal opacity.
  - **Main Menu Background Restored**: fixed a hook that made the menu background render black.
  - **Smooth Camera Sensitivity**: Adjust camera look speed on the fly with **Select + D-Pad Up / Down** (levels 1–10). An on-screen visual indicator appears, and settings are saved to `ux0:data/brothersinarms2/camera_sens.txt`.
  - **Intro Video Playback**: Unconverted `intro.mp4` (H.264 Baseline + AAC) plays on boot using the PS Vita's hardware video decoder (`sceAvcdec`) and software AAC audio decoding via FFmpeg (`libavcodec`). Can be skipped anytime with **Cross** or **Start**.
  - **Audio System**: Custom 48 kHz stereo mixer routing sound effects and Vorbis OGG music to `sceAudioOut`.
  - **Data Paths**: Remapped all save and asset directories to `ux0:data/brothersinarms2/`.

---

## Setup Instructions

To run this port, you must supply assets from a legally obtained copy of the Android game:

1. Ensure [**kubridge**](https://github.com/TheOfficialFloW/kubridge/releases) and [**FdFix**](https://github.com/TheOfficialFloW/FdFix/releases) are installed on your PS Vita (`ur0:tai/config.txt` under `*KERNEL`).
2. Ensure [**libshacccg.suprx**](https://github.com/Rinnegatamante/ShaRKBR33D/releases/latest) is installed in `ur0:data/`.
3. Obtain the original APK of **Brothers in Arms 2: Global Front HD v1.0.4** (`Brothers_In_Arms_2__Global_Front_HD_1.0.4.apk`) and its data cache folder (`BIA2/`).
4. Extract the `.apk` using any zip archive tool on your PC.
5. Copy `lib/armeabi-v7a/libbia2.so` from inside the APK to `ux0:data/brothersinarms2/libbia2.so`.
6. Copy the APK's `res/raw/` directory contents to `ux0:data/brothersinarms2/res/raw/`.
7. Copy the complete `BIA2/` data cache folder (`intro.mp4`, `sprites/`, `sounds/`, `gui/`, etc.) to `ux0:data/brothersinarms2/BIA2/`.
8. Install `brothersinarms2.vpk` on your PS Vita using **VitaShell**.
9. Launch the game from your LiveArea.

### Directory Layout on Memory Card

```text
ux0:data/brothersinarms2/
├── libbia2.so            <- Extracted from lib/armeabi-v7a/ in the APK
├── BIA2/                 <- Android data cache folder (gameloft/games/bia2)
│   ├── intro.mp4         <- Boot intro video
│   ├── sprites/
│   ├── sounds/
│   ├── fonts/
│   └── gui/
├── res/
│   └── raw/              <- Extracted from res/raw/ in the APK (raw_000.ogg, etc.)
├── saves/                <- Created automatically by the game
├── logs/                 <- Debug logs (brothersinarms2_NNN.log)
└── camera_sens.txt       <- Camera sensitivity setting file
```

---

## Controls Reference

- **Left Stick** — Move soldier
- **Right Stick** — Camera look / Aim (light tilt = precise aim, full tilt = fast turn)
- **R / R1 / RTRIGGER** — Fire weapon (hold)
- **L / L1 / LTRIGGER** — Aim / Iron sights (toggle)
- **Cross** — Context action (jump, climb, interact)
- **Circle** — Run / dash (hold in game; Back in menus)
- **Select** — Shake (melee "SHAKE" prompts; shaking the console also works)
- **Square** — Change stance (Stand / Crouch / Prone)
- **Triangle** — Switch weapon / Reload
- **Start** — Pause / Menu
- **Select + D-Pad Up / Down** — Adjust camera sensitivity (1 to 10)
- **D-Pad (without Select)** — Move soldier / Navigate menus
- **Touch Screen** — Touch controls (still responsive; bottom buttons at 1% opacity)

---

## Troubleshooting

- **Error dialog on boot saying data files are missing:**
  Ensure `ux0:data/brothersinarms2/libbia2.so` is present and that `ux0:data/brothersinarms2/BIA2/` contains the cache files.
- **Crash on startup or black screen:**
  Verify that `kubridge.skprx` and `fd_fix.skprx` are loaded under `*KERNEL` in `ur0:tai/config.txt`, and that `ur0:data/libshacccg.suprx` is present.
- **If an issue occurs:**
  Check the latest log created in `ux0:data/brothersinarms2/logs/brothersinarms2_NNN.log`.

---

## Credits

- **Gameloft & Gearbox Software** for the original game.
- **TheFloW** for `so_util`, `kubridge`, and `FdFix`.
- **Rinnegatamante** for `vitaGL`.
- **v-atamanenko** for `FalsoJNI` and `soloader-boilerplate`.
- **Wonder Diaz** for porting, reverse engineering, controls mapping, and optimization for PS Vita.
