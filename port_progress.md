# Registro de Progreso — Brothers in Arms 2 (PS Vita)

## Fase 1: Configuración y Preparación (Completada — 2026-09-08)
- Repo creado desde soloader-boilerplate, `.gitignore` anti-DMCA.
- APK `Brothers-In-Arms-2.apk` copiado y extraído.
- ABI detectada: armeabi, armeabi-v7a (elegida: armeabi-v7a).
- GLES detectado: valor no estándar en manifest: 0x20000 (declarado en AndroidManifest.xml)

## Fase 2: Decompilación (Completada — 2026-09-08)
- jadx: corrido, resultados en decompiled/apk_jadx/.
- Ghidra (.so): NO corrido (docker/imagen no disponibles) -- pendiente.

## Fase 3: Análisis del Motor Real (Pendiente)
- [ ] Confirmar si comparte motor con algún port hermano.
- [ ] Leer decompiled/apk_jadx/sources/ para el ciclo de vida nativo real.
- [ ] Confirmar exports JNI reales y si hay RegisterNatives.

## Fase 4 en adelante: Pendiente
Ver PORTING_PLAN.md sección 4. Actualizar con un bug confirmado a la vez en pruebas reales.

---

> **Corrección 2026-09-24:** las Fases 1-2 de arriba se hicieron sobre otro APK
> (`ANMP.GloftB2HM`, `libnativedummy.so` vacío). El port usa ahora el APK
> `Brothers_In_Arms_2__Global_Front_HD_1.0.4` (paquete `GAND.GloftBia2.bia2`) + el cache
> `Brothers-In-Arms-2-v1-0-4-UKR-RUS-cache.zip`. Ghidra ya corrido sobre los 3 .so.

## Fase 3: Análisis del Motor Real (Completada — 2026-09-24)
- `libbia2.so` = motor (GLES 1.1, softfp). `libStormGLOFT.so` = shim de hooks de terceros
  (shellcode sobre GL/touch + glsl-optimizer) → **no se carga**.
- Motor hermano de **Asphalt 5** (wrapper Gameloft GAND 2010): diff de exports `Java_*` 1:1 con
  clases renombradas. Sin `JNI_OnLoad`/`RegisterNatives`.
- Ciclo de vida confirmado en `GameRenderer.onSurfaceCreated()` (jadx) y en el pseudo-C de cada
  native (firmas, static vs. instance). Métodos Java pedidos: 8 (BIA2) + 31 (GLMediaPlayer) +
  5 (GLResLoader), todos `GetStaticMethodID`, sin colisiones de nombre.
- `appInit` hardcodea `/sdcard/gameloft/games/bia2/`; todo el I/O es `fopen` directo
  (`GetNativeResource*` no tiene callers).
- Pack de texturas ATC (`res/raw/data.txt`); `Texture::LoadATCFromBuffer` → vitaGL decodifica ATC.
- `SCREEN_WIDTH/HEIGHT` default 800x480 en `.data`, static initializer los usa antes de
  `appInit`, LibEffects usa RT fijo 1024x512 → reportar 800x480.

## Fase 4: Bootstrap del loader (Completada — 2026-09-24, sin probar en consola)
- `lib/falso_jni` (submódulo vacío) y `lib/vitagl` clonados desde los de Asphalt-5-Vita
  (mismos commits: FalsoJNI `083d5a0`, vitaGL `3ed4c9e`). `lib/stb/stb_vorbis.c`.
- `dynlib.c`: 163/163 imports. Agregados `__assert`, `__dso_handle`, `bzero`,
  `getprotobyname` (struct bionic), `inet_addr`; `sigaction` → `ret0` (solo SIGPIPE, struct
  bionic ≠ newlib); `remove` → `remove_soloader`.
- `reimpl/io.c`: `vita_path()` reescribe `/sdcard/gameloft/games/bia2` y `/data/data/<pkg>` →
  `ux0:data/brothersinarms2/` en `fopen`/`open`/`stat`/`remove`.
- `main.c`: ciclo de vida de `onSurfaceCreated`, idioma del sistema → `BIA2.iG`, chequeo de
  datos con diálogo, log si `ApplicationImpl::Init` falla (`mbAppRequestExit`), PowerTick,
  stack 4 MB.
- `patch.c`: `SCREEN_WIDTH/HEIGHT = 800x480` antes del INIT_ARRAY.
- `java.c` + `jni_bia2.c`: tabla JNI real. Exit → `sceKernelExitProcess`, IsWifiEnabled → 0,
  getHostName → "PSVita_PSVita", IGP/GLLive/market → no-op, `loadMovie` → se omite (el motor
  no espera al video), GLResLoader con archivos.
- `audio.c`: semántica de GLMediaPlayer.java/ac.java reproducida (isSoundLoaded 0/-1,
  una voz por sonido, pitch de playSound sin efecto, estados de MediaPlayer, GLAsyncPlayer).
  Mixer 48 kHz estéreo, remuestreo lineal (los .ogg son 22050 mono / 44100 estéreo), SFX
  > 1 MB decodificados se streamean.
- `input.c`: touch frontal → 800x480, slots 0-4 (nunca el id crudo), Círculo=BACK(4), Start=MENU(82).
- `utils/glutil.c`:
  - `vglInitExtended(960x544, 12 MB, sin MSAA)` + `glHint(GL_TEXTURE_COMPRESSION_HINT, GL_NICEST)`
    (ATC → DXT en vez de RGBA8).
  - `glViewport`/`glScissor` escalados 800x480→960x544 solo con FB 0; re-aplicados en cada
    `glBindFramebuffer` (LibEffects setea el viewport *antes* de bindear FB 0).
  - **Bug evitado (confirmado leyendo vitaGL):** con `NO_DEBUG=1` vitaGL salta el `if (!fb)` de
    `glFramebufferRenderbuffer` → NULL deref cuando `LibEffects::BeginRenderTargetGroup(NULL)`
    engancha depth al FB 0. Guard en `glFramebufferRenderbuffer/Texture2D_soloader`.
  - `glGetFramebufferAttachmentParameteriv` solo soporta COLOR_ATTACHMENT0 en vitaGL (no escribe
    `*params`) → wrapper devuelve 0.
  - ATC: `atitc_decode` no decodifica mips < 4 px → `glCompressedTexImage2D_soloader` los omite
    (y redondea el nivel 0 a bloques 4x4).
- Logger de A5 (archivo por ejecución `logs/brothersinarms2_NNN.log` + netlog UDP opcional).
- LiveArea generada desde `res/drawable/icon.png`, validada (8-bit indexado).
- Build: `.gitignore` tenía `Makefile` sin anclar → el rsync del toolkit excluía
  `lib/vitagl/Makefile` ("No makefile found"). Anclado a `/Makefile`. EGL reimpl de A5
  (el vitaGL vendorizado no trae `eglBindAPI` y otros 4).
- **Build OK**, VPK ~720 KB. Stubs autogenerados del APK viejo eliminados.
- `extras/scripts/upload_data.sh`: sube `main.so` + `BIA2/*` por FTP.

## Fase 5: Primer arranque en consola (Pendiente — requiere hardware)
Pasos en PORTING_PLAN.md §5. Riesgos a vigilar en el log: §4.

## Fase 4b: Layout de datos `ux0_data/brothersinarms2/` (2026-09-24, sin probar en consola)
- Estructura definida por el usuario: `libbia2.so`, `BIA2/`, `res/`, `saves/`, `logs/`.
- CMake: `SO_PATH=…/libbia2.so`, nuevos `GAME_PATH` (BIA2/), `SAVE_PATH` (saves/), `RES_PATH` (res/).
- `vita_path()`: datos → `BIA2/`; saves (`BIA2.data`, `BIA2_bak.data`, `profile.sav`,
  `serverConfig.sav`, `oconf.bar` — los únicos archivos que escribe el motor) → `saves/`, con
  lectura de semilla desde `BIA2/` mientras no exista la propia. Probado en host con un harness.
- `GLResLoader.getSoundRaw/getResourceLengthSoundRaw` leen `res/raw/raw_%03d.ogg`.
- Logger: se eliminó `logs/next.idx`; el índice sale del `.log` modificado más recientemente
  (+1, 999→001), así `logs/` solo contiene `brothersinarms2_001.log`…`_999.log`.
- `upload_data.sh` replica el layout, salta `libStormGLOFT.so`, `._*`, `.gitkeep`, y nunca sube `saves/`/`logs/`.
- Build OK.

## Fase 5: Primer arranque en consola real (2026-09-24)

### Bug #1 — pantalla negra tras el logo Ubisoft/Gearbox, consola congelada (ni el botón PS)
**Síntoma (usuario):** negro → logo Ubisoft/Gearbox → negro para siempre; el botón PS no
responde, hubo que forzar el apagado.

**Log `brothersinarms2_001.log` (70 líneas):** arranque completo sin errores — .so cargado,
163 imports, los 4 `nativeInit`, `GameRenderer.nativeInit` OK, `g_appPath` correcto, lecturas de
`BIA2.data`, `sprites`, `TEXT_EN` (idioma 4 = español → carga TEXT_EN? revisar), `fonts`, `gui`,
`intro.mp4` omitido, 4 lecturas más de `sprites` (≈ logo), y la última línea a los 14.77 s:
`audio: sound pool destroyed.` (`BaseSoundManager::allocBuffers()`), **sin** el
`sound pool ready.` que el motor dispara inmediatamente después (`nativeInitSoundPoolArray`).

**Diagnóstico — lo confirmado:**
- El log se escribe sin buffer (sobrevive a un crash del proceso), pero en exFAT el tamaño del
  archivo solo se confirma al cerrar/sincronizar: con el apagado forzado **se perdió la cola del
  log**. La última línea visible no es necesariamente dónde se colgó.
- "Botón PS muerto" = el shell no puede suspender la app: típico de un hilo trabado en una espera
  no interrumpible (GPU / sync de vitaGL), no de un loop normal del motor.
- Descartado: `allocBuffers` hace `loadSound` de los ids 91–193 en el hilo de render, pero son
  105 s de audio (≈ segundos de decodificación), no un cuelgue permanente. `WorkThread`/
  `WorkThread2` solo guardan partida. No hay deadlock posible en `audio.c` (ningún lock anidado,
  el mixer no loguea bajo `s_lock`).
- Sin causa raíz todavía: faltan datos → **build de diagnóstico** (no se adivina un fix).

**Cambios (para que no vuelva a congelar la consola y para ver la causa):**
- `utils/logger.c`: `sceIoSyncByFd` cada ≤ 250 ms y siempre en WARN/ERROR/FATAL → la cola del
  log sobrevive a un apagado forzado. `log_sync()` público.
- `source/watchdog.c` (hilo prioridad 0x40, independiente del loop del juego):
  - **SELECT + START 2 s → sale de la app** aunque el motor nunca retorne.
  - Heartbeat 1/s: frames, `g_dwCurrentFrm`, estado del motor (`s_game+0x140`), fase del loop
    (`input`/`nativeRender`/`swap`), memoria libre de vitaGL (VRAM/RAM/PHYCONT) y newlib usada;
    y cada cambio de estado del motor apenas ocurre.
  - Si el loop no avanza en **30 s**: loguea fase/estado, sincroniza y **provoca un data abort a
    propósito** → la Vita escribe un `.psp2dmp` con los registros de todos los hilos (dónde está
    trabado el principal) y cierra la app en vez de dejar la consola congelada.
- `audio.c`: log DEBUG por `loadSound` con frames/Hz/tiempo de decodificación.

**Pendiente:** reproducir con este build y traer `logs/brothersinarms2_002.log` + el `.psp2dmp`
de `ux0:data/` si el watchdog lo generó.

### Bug #2 — crash a 1 s del arranque (regresión del build de diagnóstico de Bug #1)
**Log `brothersinarms2_002.log`:** se corta tras el primer heartbeat del watchdog (1.1 s), con el
hilo principal todavía dentro de `gl_init()` (en el primer arranque tardó ~4.5 s).
**Dump (`psvita-toolkit analyze`):** hilo en crash **`bia2_watchdog`**, data abort con
LR = `vgl_mem_get_free_space` (`lib/vitagl/source/utils/mem_utils.c:630`), llamado desde
`watchdog_thread` (`watchdog.c:95`) → `vglMemFree()` sobre los heaps de vitaGL mientras
`vglInitExtended` los estaba creando en el hilo principal. Causa: mi watchdog, no el juego.
**Fix:** la memoria (vitaGL + `mallinfo`) la mide el hilo principal en `watchdog_frame()` (1/s,
siempre después de `gl_init`) y el watchdog solo imprime los últimos valores (atomics). El
watchdog ya no toca vitaGL ni malloc. Fases `gl_init`/`audio_init` agregadas. Build OK.
**Pendiente:** Bug #1 sigue sin datos — volver a probar.

### Bug #3 — GPU crash tras el sound pool (frame 335, `OGL2DFlush`/`glDrawElements`)
**Log `brothersinarms2_003.log`:** esta vez el arranque pasó el punto del Bug #1 — sound pool
`destroyed` → `ready`, 103 sonidos decodificados (ids 91–193), cambio de estado `0 -> 2` en el
frame 335 (25.96 s). Ahí se corta el log.
**Dump (`GPUCRASH`, stop reason `0x0` = ejecución normal, no un fault):** hilo principal en
`OGL2DFlush()+0x134` → `OGL2DEndDraw()` → `vglSwapBuffers` (`gxm.c:928`) → `Main::run()+0x1c4`.
`OGL2DFlush` (pseudo-C `out_ghidra.c:27396`) es un batch 2D chico (≤200 quads) que termina en un
único `glDrawElements` — el mismo punto donde el port hermano **Asphalt 5** tuvo sus GPU crashes
(Bugs #8/#10/#23 de su bitácora, mismo motor Gameloft GAND 2010).

**Causa (por inspección, mismo patrón que A5 Bug #8):** `glutil.c` escalaba cada
`glViewport`/`glScissor` de 800x480 → 960x544 sobre el framebuffer real siempre que el FB
bindeado era el 0, sin clampear. Pero el viewport es estado global y el motor setea viewports
de sus propios render targets (LibEffects RT fijo 1024x512) que pueden seguir pendientes cuando
se re-bindea el FB 0 → rect fuera de rango para GXM → la GPU rechaza el estado (GPU crash).
Además faltaba el wrapper de A5 Bug #23 (`glTexImage2D` con internalformat DXT + píxeles sin
comprimir → `dxt_compress()` síncrono por software, segundos de stall triageado como GPU crash).

**Fix (heredado de Asphalt 5, adaptado a que este motor SÍ usa sus propios FBOs):**
- `gl_init_downsample()`: FBO offscreen 800x480 (== lo que se le reporta al motor, identidad sin
  redondeo) + depth, queda bindeado; `gl_swap()` hace un blit de upscale a 960x544 con
  save/restore completo de estado (lección de A5 Bug #15) y re-bindea el FBO.
- `glBindFramebuffer_soloader`: bind de 0 → `s_ds_fbo`, binds ≠0 (RTs de LibEffects) intactos;
  `GL_FRAMEBUFFER_BINDING` devuelve el número del motor, no el real.
- `glViewport/Scissor`: identidad + clamp a OFFSCREEN cuando el target es el FB del motor;
  passthrough para los FBOs propios del motor. Guards de FBO/attachment ahora sobre el FB del
  motor. Se conserva el wrapper ATC.
- `glTexImage2D_soloader` (nuevo, = A5 Bug #23): internalformat DXT/SRGB-comprimido → `GL_RGBA`.
- Sombras `glEnable/Disable(_soloader)` para el blit; `dynlib.c` rutea `glTexImage2D`,
  `glEnable/Disable`, `glEnable/DisableClientState` a los wrappers.
- ~26% menos píxeles rasterizados por frame como efecto lateral (igual que A5 Bug #10).
Build release OK. **Pendiente:** desplegar (`deploy --eboot` o `--vpk`), reproducir y traer
`logs/brothersinarms2_004.log` (+ `.psp2dmp` si vuelve a caer).

### Bug #4 — pantalla roja titilante, luego negra con música; stall en `swap` (frame 345+)
**Síntoma (usuario, build Bug #3):** rojo titilante → logos Ubisoft/Gearbox reales → negro con
música de fondo (posible título); PS suena a salida pero la imagen negra no se va → cierre forzado.
**Log `brothersinarms2_004.log`:** Downsample FBO OK, logos y sound pool bien, estado `0 -> 2`
(frame 345) y de ahí frames congelados en fase `swap` hasta que el watchdog provoca el fault a
los 30 s (línea FATAL 50.8 s). O sea: `nativeRender` retorna pero `gl_swap()` no — cuelgue de GPU
cuyo veneno se encoló en el draw anterior (GXM es asíncrono; el wait revienta en el swap).
Música viva = hilo de audio independiente; PS medio muerto = firma clásica de GPU hang (ver Bug #1).

**Descartado en el .so:** no usa `glDrawTex*OES` (0 callers — los `ret0` de dynlib son inocuos)
ni VBOs (`BindBuffer` 0 resultados) → el blit con punteros CPU es seguro. Confirmado en cambio
que usa multitextura (`SetActiveTextureSlot`, `g_ClientActiveTexture`, 35 `ActiveTexture`):
al momento del swap la unidad activa (servidor y cliente) muchas veces NO es la 0.

**Causa del rojo/negro (por inspección):** el blit bindeaba `s_ds_color_tex` y seteaba
texcoords sobre *la unidad que hubiera activa*, mientras la unidad 0 conservaba el COMBINE del
motor (quad rojo) o nada sampleable (quad negro). Los logos se veían porque ahí la unidad
activa aún era la 0.
**Fix:** el blit fija `glActiveTexture`/`glClientActiveTexture` a `TEXTURE0` (con save/restore)
antes de todo. No se tocó LibEffects todavía (un bug a la vez).
Build release OK. **Pendiente:** desplegar, probar y traer `brothersinarms2_005.log`; si se
repite el stall, también el `.psp2dmp` nuevo de `ux0:data/` (el watchdog lo genera a propósito
— su PC dice si el cuelgue está en el draw del blit o en el wait de `vglSwapBuffers`).

### Bug #5 — stall en `swap` al entrar al 3D (estado 2): tres causas sospechadas
**Log `brothersinarms2_004.log`, revisado de nuevo.** El fix del Bug #4 explicaba el rojo/negro,
no el cuelgue de la GPU. **No hay `.psp2dmp` del watchdog** (no se bajó de la consola, que no
respondía por FTP), así que esto sale de leer el código; no está confirmado en la consola.

1. **Orden viewport/bind en LibEffects (bug real, visto en el pseudo-C):**
   `Manager::BeginRenderTargetGroup` (`out_ghidra.c:~282325`) llama
   `glViewport(0,0,rt_w,rt_h)` **antes** de `glBindFramebufferOES(rt)`. El wrapper decidía si
   clampear según el FB bindeado *en el momento del glViewport* → el viewport del RT 1024x512 de
   la escena se clampeaba a 800x480. **Fix:** `glutil.c` guarda los rects crudos del motor y los
   vuelve a aplicar contra el target real en cada bind (`bind_engine_target()`);
   `glGetIntegerv(GL_VIEWPORT/GL_SCISSOR_BOX)` devuelve el rect del motor.
2. **`gl_swap()` re-bindeaba siempre `s_ds_fbo`**, aunque el motor creyera que tenía un RT
   bindeado (`Manager::Begin` deja el RT de la escena bindeado). Eso dejaba
   `FRAMEBUFFER_BINDING` fuera de sincronía con el FBO real. **Fix:** después del swap se re-bindea
   el target que el motor tiene bindeado.
3. **`DRAW_SPEEDHACK=2` en vitaGL:** los streams de vértices de más de 32 KB (solo los draws 3D,
   que empiezan justo en el estado 2) iban a la GPU desde la memoria del motor sin copiarse, así
   que la GPU podía leerlos después de que el motor ya los hubiera reescrito o liberado. A5 nunca
   lo confirmó en consola, y el vitaGL actual tiene un pool circular de 32 MB con fallback.
   **Fix:** se sacó de `VITAGL_MAKE_FLAGS`.
4. **Diagnóstico:** `gl_swap()` ahora marca las fases `swap:blit` / `swap:vglSwapBuffers` /
   `swap:rebind`, así la línea FATAL del watchdog dice en qué espera se colgó.

Build release OK (vitaGL recompilado). **Pendiente:** desplegar, probar y traer
`brothersinarms2_005.log` + el `.psp2dmp` de `ux0:data/` si vuelve a colgarse.

### Bug #6 — CAUSA ENCONTRADA del cuelgue de GPU en la pantalla de título: `glBindTexture(GL_TEXTURE_2D, 0xFFFFFFFF)`
**Log `brothersinarms2_005.log`** (build del Bug #5): mismo cuelgue, frame 346, primer frame del
estado 2, ahora acotado a `swap:vglSwapBuffers`. El PS tampoco sacaba del juego → apagado forzado.
**Dump** `logs/brothersinarms2-psp2core-1790644786-...psp2dmp` (el fault del watchdog SÍ se
escribió): el hilo principal está en una espera del kernel **dentro de SceGxm** (llamado desde
`vglSwapBuffers`), y `SceGxmDisplayQueue` también está esperando. O sea, la GPU nunca termina el frame.

**Causa (pseudo-C + fuente de vitaGL):** el estado 2 (`Main::Render`, `case 2`) es la pantalla de
título, solo 2D. Lo nuevo respecto de los logos es `DrawFillRect()` (el panel translúcido), que hace
`OGL2DBindTexture(0xffffffff, 1, 1)`; `OGL2DFlush` → `EnableTexMap(0, 0xffffffff)` →
**`glBindTexture(GL_TEXTURE_2D, 0xFFFFFFFF)` con GL_TEXTURE_2D habilitado**. En GLES eso es una textura
incompleta: la unidad se comporta como deshabilitada y el quad sale con su color de vértice. vitaGL con
`NO_DEBUG` se salta su chequeo `texture >= TEXTURES_NUM` (`textures.c:1778`), guarda el id, y el
camino de función fija (`ffp.c`, sin chequeo de `TEX_VALID`) pasa `texture_slots[0xFFFFFFFF]`
(= la memoria justo *antes* de la tabla) a `sceGxmSetFragmentTexture` → un descriptor basura →
la GPU se cuelga.

**Fix (`glutil.c` + `dynlib.c`):** wrappers para `glBindTexture` / `glActiveTexture` / `glIsEnabled`.
Un bind fuera de rango bindea 0 y deshabilita de verdad GL_TEXTURE_2D en esa unidad (la semántica
de GLES), mientras el motor sigue viendo el estado que él mismo seteó. Al bindear una textura válida
se re-habilita si el motor la tenía habilitada. Se loguea una sola vez (`l_warn`).
**Nota PS-button:** con la GPU colgada el kernel no puede terminar el proceso (ni el PS ni el fault del
watchdog), así que la única solución real es que no se cuelgue.
Build release OK, `eboot.bin` desplegado. **Confirmado en `brothersinarms2_006.log`:** juego jugable, ingresa al menú y al gameplay 3D con buen rendimiento.

### Release v1.0 — Texturas, Controles Físicos, Opacidad 1% y Video Intro (2026-09-30)

1. **Texturas azules solucionadas (ATC Software Decoder):**
   - El juego usa compresión Qualcomm/AMD ATC (`GL_ATC_RGB_AMD` 0x8c92, `GL_ATC_RGBA_EXPLICIT_ALPHA_AMD` 0x8c93).
   - En vitaGL, `atitc_decode` decodifica con canales rojo y azul invertidos (RGB<->BGR), provocando que la piel y la madera de las armas se vean azules/cian.
   - **Solución:** Implementado decodificador ATC por software en `source/reimpl/atc.c` (adoptado de N.O.V.A 2). Decodifica a `RGB565` y `RGBA8888` respetando los canales correctos y permitiendo cualquier dimensión de mips.

2. **Controles Físicos Completos (Dual Sticks y Botones Vita):**
   - El motor es 100% táctil en gameplay (`BIA2_nativeTouch*`). Se implementó un sistema multi-slot sintético (8 slots) en `source/input.c`:
     - **Stick Izquierdo / D-Pad**: Movimiento del soldado inyectado sobre el joystick virtual en `(121, 354)` (slot 3).
     - **Stick Derecho**: Cámara por velocidad continua con arrastre y wrap en caja `[420..620, 140..340]` (slot 4).
     - **Sensibilidad de cámara (1-10)**: Regulable en tiempo real con `Select + D-Pad Arriba/Abajo`, guardada en `camera_sens.txt` con barra OSD visual.
     - **R / RTRIGGER / R1**: Disparo continuo en `(660, 356)` (slot 5).
     - **L / LTRIGGER / L1**: Modo mira / Iron sights (toggle con tap) en `(782, 469)`.
     - **Cross**: Acción contextual (trepar, saltar, vehículos) en `(398, 49)`.
     - **Circle**: Sprint / Melee en gameplay en `(762, 272)` (slot 6); BACK en menús (`AKEYCODE_BACK`).
     - **Square**: Cambio de postura (de pie / agachado / cuerpo a tierra) en `(17, 456)`.
     - **Triangle**: Cambio de arma / recarga en `(689, 35)`.
     - **Start**: Pausa / Menú en `(43, 39)` y `AKEYCODE_MENU`.
     - **Pantalla táctil**: Mapeada en slots 0..2 para uso directo en cualquier momento.

3. **Opacidad del 1% para Controles Inferiores:**
   - Para no saturar la pantalla durante el juego físico, los botones virtuales inferiores fueron atenuados al 1% de opacidad:
     - Multiplicador maestro de opacidad del motor `*(float *)(Main + 0x1b5c) = 0.01f;` fijado en cada frame (`patch_dim_tick()`).
     - Hook en `CInputJoystick::RenderJoystickButton` (`0x00117604`) forzando alpha a `0.01f`.
     - Hook en `Main::RenderFireButton` (`0x0011aa38`) forzando alpha a `0.01f`.
     - Hook en `Main::PaintGraphItem` (`0x00305774`) para el grupo HUD `0x10`, forzando `alpha = 0.01f` en postura (`0x36/0x37`), sprint (`0x31..0x35`) y mira (`0x30`).
     - El HUD superior (pausa, flecha verde de objetivo, munición) conserva su visibilidad normal.

4. **Reproducción del Video Intro (`intro.mp4`):**
   - El archivo `intro.mp4` es H.264 Baseline + AAC mono.
   - Implementado reproductor con demuxer ISO-BMFF propio + decoder por hardware de la Vita `sceAvcdec` (`source/video_avc.c`) y decodificación de audio AAC con FFmpeg (`libavcodec`).
   - Hookeado en `GLMediaPlayer_loadMovie("intro.mp4")` en `source/jni_bia2.c`. Se puede saltar con Cross o Start.

5. **Release v1.0 Preparado:**
   - Compilación limpia de `brothersinarms2.vpk` y `eboot.bin`.
   - `README.md` completo al estándar de los demás ports de Gameloft en PS Vita.
   - `RELEASE.md` listo para publicación en GitHub.

### Bug #7 — Log `brothersinarms2_007.log`: video, menú negro, cámara, correr, SHAKE (2026-09-30)

1. **Video intro no se reproducía.** `sceVideodecInitLibrary -> 0x80024302` (SCE_KERNEL_ERROR_NO_FREE_PHYSICAL_PAGE)
   y el fallback SceAvPlayer tampoco pudo alojar su textura (CDRAM 0x80024309 / PHYCONT 0x80024302):
   vitaGL se quedaba con toda la PHYCONT. **Fix** (mismo esquema que N.O.V.A 2): `gl_init()` pasa
   `phycont_threshold = 0x7FFFFFFF` (PHYCONT fuera de vitaGL) y `GLMediaPlayer_loadMovie` llama
   `gl_provide_phycont()` después del video -- esa llamada faltaba.
2. **Menú principal negro.** El hook de `Main::PaintGraphItem` (`_ZN4Main14PaintGraphItemEiiiif` =
   this, group, item, x, y, float alpha) omitía `y` y llamaba al original vía `SO_CONTINUE` sin prototipo
   (float -> double): el original leía alpha basura. `RenderMenuBG` dibuja con `PaintGraphItem(0, i, x, 0, 1.0f)`
   -> fondo invisible. **Fix:** firma completa + puntero con prototipo (`patch.c`).
3. **Cámara limitada.** El "wrap" de `syn_look` soltaba y re-apretaba el MISMO id en un frame: ambos eventos caen
   en el mismo slot del motor, y `Main::PostProcessKeys` mata el press nuevo (flag de release +0x874 aún puesto)
   -> la cámara se frenaba hasta centrar el stick. **Fix:** el wrap alterna entre dos ids (4 y 8), caja más grande
   (±180 x ±150) y velocidad máx. 24/14 px/frame (la ganancia del motor satura en 16 px/frame).
4. **CIRCLE no corría y abría el menú.** No hay botón de sprint en (762,272): correr = mantener el botón HUD "dash"
   (`GetAreaTouchId(0x10, 9)` -> `MC::SetDashMode(1)`). **Fix:** el centro del rect se lee en runtime con
   `GUILevel::GetParamValue(hud, 9, 2/3/5/6)` (base 480x320) y CIRCLE lo mantiene; en misión (estado 0x14)
   ya no manda `AKEYCODE_BACK` (START pausa). Fuera de misión sigue siendo BACK.
5. **SHAKE imposible.** `BIA2_nativeAccelerometer` nunca se llamaba (valores en 0). El QTE de melee
   (`Main::ProcessPhoneGesturesInGame`: |Δz| > 0.3 m/s² en un frame -> `IsPhoneShaked`) no se podía pasar.
   **Fix:** acelerómetro real (sceMotion, g -> m/s², ejes a teléfono en landscape) cada frame + **SELECT = shake**
   sintético (16 frames alternando z ±6). Ejes X/Y (planeador) sin verificar en consola: se loguea la primera muestra.

Build OK, `eboot.bin` desplegado. **Pendiente:** probar y traer `brothersinarms2_008.log`
(buscar `video_avc: primer frame`, `dash button rect`, `motion first sample`).

### Bug #8 — Log `brothersinarms2_008.log`: FPS, opacidad del HUD superior (2026-09-30)

Confirmado en 008: video intro OK (221 frames, PHYCONT 25600 KB devuelta a vitaGL), dash leído
(`x=418 y=156 w=60 h=60 -> (746,279)`), acelerómetro vivo (`g=(0.06, 0.16, -1.06)`).

1. **HUD superior (pausa, panel del arma) casi invisible.** `Main+0x1b5c` es la opción "opacidad de controles"
   del juego (se guarda en profile.sav) y el desensamblado la usa (`0x1b40 + 28`, Ghidra no la mostraba) como
   alpha del sprite del HUD en todo `PaintIGNormal`. `patch_dim_tick()` la forzaba a 0.01 cada frame.
   **Fix:** 0.01 solo durante `RenderJoystickButton` / `RenderFireButton` y se restaura; si el profile quedó con
   <0.3 (resto del bug) vuelve a 1.0. Ítems HUD 0x30..0x37 al 1% salvo que estén en la franja superior.
2. **FPS en misión ~15-25 (006: 40-60).** Cambios:
   - ATC ya no se expande a RGB565/RGBA8888: cada bloque ATC se transcodifica a un bloque DXT1/DXT3/DXT5
     (mismo layout; la mitad alpha es idéntica) -> 4/8 bpp en VRAM y en ancho de banda de GPU.
   - Hooks por trampolín (2 instrucciones del prólogo ARM reubicadas + salto) en vez de SO_CONTINUE
     (4 llamadas al kernel por llamada, y PaintGraphItem corre por cada ítem del HUD cada frame).
   - vitaGL: `MATH_SPEEDHACK=1 SAMPLERS_SPEEDHACK=1` (set CPU-only de N.O.V.A 2). Excluidos NO_TEX_COMBINER
     (el motor usa GL_COMBINE) y CIRCULAR_POOL/DRAW_SPEEDHACK (riesgo de cuelgue de GPU, Bug #5).
   - `vglUseCachedMem(GL_TRUE)` como N.O.V.A 2.

Build OK, `eboot.bin` desplegado. **Pendiente:** medir FPS en misión (`brothersinarms2_009.log`).
