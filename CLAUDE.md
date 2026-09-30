# Brothers in Arms 2 — Port a PS Vita

Port de `Brothers-In-Arms-2.apk` (Android) a PS Vita vía soloader. Generado con **psvita-port-toolkit**.

## Estructura

- `brothersinarms2_extract/` — APK extraído (gitignored).
- `decompiled/` — Java (jadx) y pseudo-C (Ghidra) del/los .so (gitignored, regenerable).
- `source/`, `lib/so_util`, `lib/falso_jni` — scaffold del boilerplate (SoLoader + FalsoJNI).
- `PORTING_PLAN.md` — plan vivo, actualizar a medida que se confirman cosas del motor real.
- `port_progress.md` — bitácora, un bug confirmado a la vez.
- `.psvita-toolkit.json` — config para el toolkit standalone (build/deploy/logs/LiveArea/crash dumps).

Este port **no** tiene una copia local de `porting_tools/` -- todo el build/deploy/debug se maneja
desde **psvita-port-toolkit**, la herramienta standalone (fuera de este repo). Abrí el toolkit y
elegí "Continuar con un port existente" apuntando a esta carpeta.

## Hallazgos de motor

Confirmados 2026-09-24 (ver PORTING_PLAN.md §1-2):

- APK 1.0.4, paquete `com.gameloft.android.GAND.GloftBia2.bia2`; motor `libbia2.so` (armeabi-v7a,
  softfp, GLES 1.1). `libStormGLOFT.so` es un shim de terceros y no se carga.
- Motor hermano de Asphalt 5 (`../Asphalt-5-Vita`), mismo wrapper Gameloft GAND.
- Resolución reportada 800x480, escalada a 960x544 en `utils/glutil.c`.
- Tarjeta: `ux0:data/brothersinarms2/{libbia2.so, BIA2/, res/, saves/, logs/}` = espejo de `ux0_data/brothersinarms2/`.

## Flujo de trabajo esperado

1. Análisis de símbolos antes de tocar loader/source -- skill `psvita-port-init` cubrió la Fase 0-2.
2. Bootstrap del loader guiado por la skill `psvita-porting`.
3. Build/deploy con el toolkit standalone → probar en consola real.
4. Un bug a la vez, guiado por el log real -- skill `so-crash-triage`.
5. Actualizar `port_progress.md` con cada bug confirmado.
