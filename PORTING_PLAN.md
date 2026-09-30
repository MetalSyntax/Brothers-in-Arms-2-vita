# Plan de Port — Brothers in Arms 2: Global Front HD (PS Vita)

> Generado por psvita-port-toolkit el 2026-09-08. **Reescrito a mano el 2026-09-24** tras
> confirmar el motor real con `readelf`/Ghidra/jadx sobre el APK 1.0.4 (el análisis automático
> original era de otro APK: `com.gameloft.android.ANMP.GloftB2HM`, con un `libnativedummy.so`
> vacío — ya no aplica).

## 0. Contexto

- **Juego:** Brothers in Arms 2: Global Front HD 1.0.4 (Android 2.2)
- **APK:** `Brothers_In_Arms_2__Global_Front_HD_1.0.4_Android_2.2.apk`
- **Datos:** `Brothers-In-Arms-2-v1-0-4-UKR-RUS-cache.zip` → `gameloft/games/BIA2/` (extraído en `BIA2/`, gitignored)
- **Paquete Java:** `com.gameloft.android.GAND.GloftBia2.bia2`
- **TITLEID:** `PSVBIA002`
- **Pack de texturas:** ATC (Adreno, Xperia Arc SO-01C) — `res/raw/data.txt`.

## 1. Binarios

| .so | Rol | ¿Se carga en Vita? |
|---|---|---|
| `lib/armeabi-v7a/libbia2.so` (6.9 MB) | Motor del juego. ARMv7, softfp, GLES **1.1** fijo | **Sí** → `ux0:data/brothersinarms2/libbia2.so` |
| `lib/armeabi-v7a/libStormGLOFT.so` | Shim de compatibilidad de un tercero: inline hooks (shellcode) sobre `glCompressedTexImage2D`/`glTexImage2D`/`glViewport`/`glScissor`/touch + glsl-optimizer de Mesa. Convierte texturas ATC/PVRTC para GPUs modernas | **No** (lo reemplazan vitaGL + `glutil.c`) |
| `lib/armeabi/libbia2.so` | Mismo motor, ARMv5 | No |

`libbia2.so` importa 163 símbolos (libc/libm/GLES1 + OES FBO + sockets). Todos resueltos en
`source/dynlib.c` (5 agregados a mano: `__assert`, `__dso_handle`, `bzero`, `getprotobyname`,
`inet_addr`). Sin `JNI_OnLoad`, sin `RegisterNatives`: exports `Java_*` por nombre.

## 2. Motor — hermano de Asphalt 5

Mismo wrapper Gameloft GAND de 2010 que `Asphalt-5-Vita` (v1.0 en consola): mismas clases
`GLResLoader`/`GLMediaPlayer`, mismo patrón `mEnv` global, `nativeGetJNIEnv`, `appInit`/`appRender`.
Cambian nombres (`Asphalt5*` → `BIA2`/`GameRenderer`/`GameGLSurfaceView`) y la API de sonido
(Music en vez de Big). Reusado de A5: logger, netlog, EGL reimpl, vitaGL vendorizado con
`DRAW_SPEEDHACK=2`, FalsoJNI.

### Ciclo de vida (GameRenderer.onSurfaceCreated, jadx) — replicado en `source/main.c`

1. `GameRenderer.nativeGetJNIEnv(env, thiz)` → `mEnv = env` (todo lo demás lee `mEnv`).
2. `GLResLoader.nativeInit(env, clazz)` → 5 `GetStaticMethodID`.
3. `GLMediaPlayer.nativeInit(env, clazz)` → 31 `GetStaticMethodID`.
4. `BIA2.nativeInit(env, clazz)` → 8 `GetStaticMethodID`.
5. `GameRenderer.nativeInit(env, clazz, kb=0, 1, w, h, lang)` (static) → `appInit(w,h,lang)` →
   `ApplicationImpl::Init(480, 320)`.
6. Loop: `GameRenderer.nativeRender()` → `appRender()` → `ApplicationImpl::Run()`.

Input: `BIA2.nativeTouchPressed/Moved/Released(x, y, id)` (instance) →
`ApplicationImpl::HandleTouchScreenEvent`, coordenadas en píxeles de `SCREEN_WIDTH x SCREEN_HEIGHT`.
`nativeSetOnKeyDown/Up(keycode)`: el motor solo reacciona a 4 (BACK) y 82 (MENU).

### Resolución

El layout es lógico 480x320 escalado por `SCREEN_WIDTH/480`, `SCREEN_HEIGHT/320`, pero:
- `.data` trae `SCREEN_WIDTH/HEIGHT = 800x480`, y un static initializer usa `SCREEN_HEIGHT` antes de `appInit`;
- LibEffects (bloom/motion blur) renderiza la escena a un RT fijo de **1024x512** → 544 de alto se recortaría.

→ Se reporta **800x480** (fijado también en `so_patch()` antes del INIT_ARRAY) y `glutil.c`
renderiza el "FB 0" del motor en un FBO offscreen de 800x480 con un blit de upscale a 960x544
en `gl_swap()` (técnica del port hermano Asphalt 5, Bugs #8/#10: escalar viewports sobre el
framebuffer real provocaba GPU crashes; ver port_progress.md Bug #3). Los FBOs propios del
motor (LibEffects) no se redirigen.

### Rutas

`appInit` hardcodea `/sdcard/gameloft/games/bia2/` (datos, saves, sonidos) y
`/data/data/<paquete>/`. Todo el I/O del motor es `fopen`/`remove` directo (los
`GLResLoader.getResource*` están registrados pero nadie los llama) → `reimpl/io.c:vita_path()`
reescribe ambos prefijos (case-insensitive). Layout en la tarjeta (espejo de `ux0_data/brothersinarms2/`):

```
ux0:data/brothersinarms2/
  libbia2.so        SO_PATH
  BIA2/             GAME_PATH  <- /sdcard/gameloft/games/bia2/*
  res/              RES_PATH   <- res/ del APK (raw/raw_000.ogg para GLResLoader.getSoundRaw)
  saves/            SAVE_PATH  <- BIA2.data, BIA2_bak.data, profile.sav, serverConfig.sav, oconf.bar
  logs/             brothersinarms2_001.log .. _999.log
```

En Android saves y datos comparten carpeta (`g_saveDirPath == g_appPath`); se separan por nombre
(los únicos archivos que el motor escribe: callers de `SaveBufferToFile`/`GetSaveFilePath`/
`GetConfigFilePath`). Lectura de una save que aún no existe en `saves/` → la del cache en `BIA2/`
(semilla, igual que en Android); toda escritura va a `saves/`. `libStormGLOFT.so` se ignora.

### Texturas

`Texture::LoadFromFile`: ATC (`0x8C92`/`0x8C93`) → `glCompressedTexImage2D`; PVR/PVRTC y raw
565/4444/5551 también soportados por el motor. vitaGL decodifica ATC por software y, con
`glHint(GL_TEXTURE_COMPRESSION_HINT, GL_NICEST)`, lo recomprime a DXT1/DXT5 (misma huella que ATC).
Mips < 4 px: vitaGL no los decodifica → `glCompressedTexImage2D_soloader` los omite.

## 3. Estado

- [x] Repo, jadx, Ghidra de los 3 .so.
- [x] Análisis del motor real (sección 2) — confirmado por símbolos/pseudo-C, no por analogía.
- [x] Loader: `main.c` (ciclo de vida), `patch.c` (SCREEN_*), `dynlib.c` (163/163 imports).
- [x] Tabla JNI (`java.c`): exactamente los 44 métodos que pide el motor.
- [x] Audio (`audio.c`): SoundPool/MediaPlayer/GLAsyncPlayer → mixer sceAudioOut 48 kHz, stb_vorbis.
- [x] Input (`input.c`): touch frontal (slots 0-4), Círculo=BACK, Start=MENU.
- [x] GL (`glutil.c`): escalado de viewport, guards de FBO sobre FB 0, ATC.
- [x] LiveArea (desde `res/drawable/icon.png`), validada.
- [x] Build OK (`psvita-toolkit build`), VPK de ~720 KB.
- [~] **Primer arranque en consola real**: arranca, muestra logo Ubisoft/Gearbox, luego negro + consola congelada (Bug #1, port_progress.md). Build de diagnóstico con watchdog (SELECT+START = salir; auto-dump a los 30 s de stall) y log sincronizado.
- [ ] Controles físicos para el FPS (stick izq. → joystick virtual, stick der. → mirar, gatillos → disparar/apuntar): requiere ver el HUD en consola.
- [ ] Video `intro.mp4` (hoy se omite; el motor no lo espera).
- [ ] Rendimiento (tiempo de carga por recompresión DXT, fps in-game).

## 4. Riesgos conocidos para el primer arranque (a mirar en el log)

1. **Memoria de texturas**: `sprites` (106 MB) + `game_textures` (31 MB) en ATC; el motor además
   guarda copia en RAM de cada mip (`LoadATCFromBuffer`). Si no alcanza: probar `GL_FASTEST`
   (sin recomprimir, más VRAM) vs. subir pools de vitaGL, o bajar `_newlib_heap_size_user`.
2. **Tiempo de carga**: la recompresión ATC→DXT es por CPU; primer arranque puede tardar.
3. **LibEffects**: `glGetFramebufferAttachmentParameteriv`/`glFramebufferRenderbuffer` sobre FB 0
   ya tienen guard; el "FB 0" del motor es un FBO offscreen 800x480 con blit en `gl_swap()`
   (Bug #3) — los RTs propios del motor (1024x512) pasan intactos. Si el bloom se ve mal,
   desactivarlo (`LibEffects::Manager +0x7a = 0`). `glTexImage2D` con internalformat DXT se
   remapea a `GL_RGBA` (compresor software `dxt_compress()`, Bug #23 de Asphalt 5).
4. **Combiners GLES1** (`GL_COMBINE`, `GL_SUBTRACT`) de LibEffects: soporte parcial en vitaGL.
5. **Excepciones C++**: `__cxa_begin_cleanup`/`__gnu_Unwind_Find_exidx` son `ret0`; un `throw` aborta.

## 5. Primer arranque — pasos

1. Requisitos en la Vita: `kubridge.skprx`, `libshacccg.suprx` (ur0:data), VitaShell con FTP.
2. `extras/scripts/upload_data.sh` → replica `ux0_data/brothersinarms2/` (libbia2.so, BIA2/, res/) en `ux0:data/brothersinarms2/`. Nunca sube `saves/`/`logs/`.
3. `psvita-toolkit deploy --vpk` → instalar `brothersinarms2.vpk` desde `ux0:downloads/`.
4. Lanzar. Log por ejecución en `ux0:data/brothersinarms2/logs/brothersinarms2_NNN.log`
   (001→999, siguiente = el último escrito + 1, vuelve a 001 después de 999; sin archivo de índice).
5. Si crashea: `.psp2dmp` en `ux0:data/` → `psvita-toolkit analyze` + skill `so-crash-triage`.

## 6. Herramientas

Gestionado con **psvita-port-toolkit** (standalone). `.gitignore` ancla `/Makefile` a la raíz:
el build del toolkit copia el árbol con rsync respetando `.gitignore`, y un `Makefile` sin anclar
excluía `lib/vitagl/Makefile`.
