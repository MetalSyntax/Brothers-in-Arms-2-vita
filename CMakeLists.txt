cmake_minimum_required(VERSION 3.14)

if (NOT DEFINED ENV{VITASDK} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR "Please define VITASDK env var to point to your SDK path or set CMAKE_TOOLCHAIN_FILE.")
endif ()

set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Toolchain file")

project(brothersinarms2 C CXX)

include("${VITASDK}/share/vita.cmake" REQUIRED)

# Vita application configuration
set(VITA_APP_NAME "Brothers in Arms 2")
set(VITA_TITLEID "PSVBIA002")
set(VITA_VPKNAME "brothersinarms2")
set(VITA_VERSION "01.00")
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

# Optional: used for "send" and "dump" targets
set(PSVITAIP "192.168.3.15" CACHE STRING "PSVita IP (for FTP access)")
set(PARSECOREPATH "$ENV{PARSECOREPATH}" CACHE STRING "vita-parse-core main.py")

# Game-specific definitions
# On-card layout (mirrors ux0_data/brothersinarms2/ in the repo):
#   libbia2.so  the engine, straight from lib/armeabi-v7a/ of the APK
#   BIA2/       the Android cache gameloft/games/BIA2/ (obj, sprites, sounds/...)
#   res/        res/ of the APK (raw/raw_000.ogg, ...)
#   saves/      BIA2.data, BIA2_bak.data, profile.sav, serverConfig.sav, oconf.bar
#   logs/       brothersinarms2_001.log .. _999.log
set(DATA_PATH "ux0:data/brothersinarms2/" CACHE STRING "Path to data (with trailing /)")
set(SO_PATH "${DATA_PATH}libbia2.so" CACHE STRING "Path to .so")
set(GAME_PATH "${DATA_PATH}BIA2/" CACHE STRING "Engine data dir, /sdcard/gameloft/games/bia2 (with trailing /)")
set(SAVE_PATH "${DATA_PATH}saves/" CACHE STRING "Save files dir (with trailing /)")
set(RES_PATH "${DATA_PATH}res/" CACHE STRING "APK res/ dir (with trailing /)")
add_definitions(-DDATA_PATH="${DATA_PATH}" -DSO_PATH="${SO_PATH}" -DGAME_PATH="${GAME_PATH}"
                -DSAVE_PATH="${SAVE_PATH}" -DRES_PATH="${RES_PATH}")

# On-device incremental log: one file per run, ux0:data/brothersinarms2/logs/
# brothersinarms2_001.log .. _999.log. Keep this ON in Release too -- it is
# the only way to see what happened before a data abort.
option(ENABLE_FILE_LOG "Write an incremental per-run log file on the Vita" ON)
if (ENABLE_FILE_LOG)
  add_definitions(-DENABLE_FILE_LOG)
endif()

# Optional UDP log sink, wire-compatible with debugnet. Leave NETLOG_IP empty
# to keep it off; it can also be enabled at runtime by creating
# ux0:data/brothersinarms2/netlog.txt (see source/utils/netlog.h).
set(NETLOG_IP "" CACHE STRING "Default UDP log receiver IP (empty = disabled)")
set(NETLOG_PORT 18194 CACHE STRING "Default UDP log receiver port")
if (NOT NETLOG_IP STREQUAL "")
  add_definitions(-DNETLOG_IP="${NETLOG_IP}")
endif()
add_definitions(-DNETLOG_DEFAULT_PORT=${NETLOG_PORT})

option(USE_SCELIBC_IO "Use SceLibcBridge for IO and some other std functions" ON)
if (USE_SCELIBC_IO)
  add_definitions(-DUSE_SCELIBC_IO)
endif()

set(SHADER_FORMAT "GLSL" CACHE STRING "Preferred shader format (one of 'GLSL', 'CG', 'GXP')")
if (${SHADER_FORMAT} STREQUAL "GLSL")
  add_definitions(-DUSE_GLSL_SHADERS)
elseif (${SHADER_FORMAT} STREQUAL "CG")
  add_definitions(-DUSE_CG_SHADERS)
elseif (${SHADER_FORMAT} STREQUAL "GXP")
  add_definitions(-DUSE_GXP_SHADERS)
else()
  message(FATAL_ERROR "SHADER_FORMAT option can only be one of: 'GLSL', 'CG', 'GXP'")
endif()

set(valid_dump_formats GLSL CG)
option(DUMP_COMPILED_SHADERS "When using GLSL or CG shader format, cache compiled shaders on disk" ON)
if (DUMP_COMPILED_SHADERS)
  if (SHADER_FORMAT IN_LIST valid_dump_formats)
    add_definitions(-DDUMP_COMPILED_SHADERS)
  else()
    message(WARNING "DUMP_COMPILED_SHADERS has no effect for the chosen shader format ${SHADER_FORMAT}")
  endif()
endif()

# Unique debug definition for loader's logging to not interfere with anything
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_definitions(-DDEBUG_SOLOADER)
  # Uncomment this for verbose FalsoJNI logging:
  #add_definitions(-DFALSOJNI_DEBUGLEVEL=0)
endif()

# makes sincos, sincosf, etc. visible
add_definitions(-D_GNU_SOURCE -D__POSIX_VISIBLE=999999)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -O3 -g -ffast-math -mfloat-abi=softfp -Wno-deprecated")

# Flags for vitaGL's own Makefile (built from lib/vitagl, not the vdpm
# prebuilt). Same set Asphalt-5-Vita ships with MINUS DRAW_SPEEDHACK=2
# (SAFER_DRAW_SPEEDHACK): with it, every vertex stream > 32 KB is handed to
# the GPU straight from the engine's own memory instead of being copied, so
# the GPU reads it asynchronously, frames later, after the engine may already
# have rewritten or freed it (CPU-skinned soldiers, per-frame 3D batches).
# Those are exactly the draws that only start with the 3D scene (engine
# state 2, BiA2 Bug #5), and A5 never confirmed the flag on hardware. The
# vitaGL in lib/vitagl has a 32 MB circular pool that falls back to a
# GC-tracked allocation on overflow, so the pool-exhaustion reason A5 had
# for the hack no longer applies.
#
# Speed (2026-09-30, log 008): the CPU-only part of the N.O.V.A 2 / DH2
# hardware-proven set. MATH_SPEEDHACK (NEON matrix math -- this engine is
# fixed-function, glMultMatrix/glRotate every draw) and SAMPLERS_SPEEDHACK.
# NOT taken: NO_TEX_COMBINER (libbia2 uses GL_COMBINE texenv, TexEnvCombine),
# CIRCULAR_POOL_SPEEDHACK (single ring the GPU may still be reading -- same
# class of GPU hang as Bug #5), DRAW_SPEEDHACK (Bug #5).
set(VITAGL_MAKE_FLAGS "SOFTFP_ABI=1 NO_DEBUG=1 HAVE_SHADER_CACHE=1 NO_SPLASHSCREEN=1 MATH_SPEEDHACK=1 SAMPLERS_SPEEDHACK=1")
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -std=gnu++20 -Wno-write-strings -Wno-psabi")

add_executable(${CMAKE_PROJECT_NAME}
               source/main.c
               source/dynlib.c
               source/java.c
               source/patch.c
               source/audio.c
               source/input.c
               source/jni_bia2.c
               source/watchdog.c
               source/video.cpp
               source/video_avc.c
               source/reimpl/atc.c
               source/reimpl/bits/_ctype.c
               source/reimpl/egl.c
               source/reimpl/errno.c
               source/reimpl/io.c
               source/reimpl/log.c
               source/reimpl/mem.c
               source/reimpl/pthr.c
               source/reimpl/sys.c
               source/reimpl/time64.c
               source/reimpl/asset_manager.cpp
               source/utils/dialog.c
               source/utils/glutil.c
               source/utils/init.c
               source/utils/logger.c
               source/utils/netlog.c
               source/utils/settings.c
               source/utils/utils.c
               lib/falso_jni/FalsoJNI.c
               lib/falso_jni/FalsoJNI_ImplBridge.c
               lib/falso_jni/FalsoJNI_Logger.c
               lib/falso_jni/converter.c
               lib/stb/stb_vorbis.c
               lib/sha1/sha1.c
               lib/fios/fios.c
               lib/so_util/so_util.c)

# vitaGL built from lib/vitagl (git submodule) with VITAGL_MAKE_FLAGS above.
separate_arguments(VITAGL_MAKE_ARGS UNIX_COMMAND "${VITAGL_MAKE_FLAGS}")

set(VITAGL_DIR ${CMAKE_SOURCE_DIR}/lib/vitagl)
set(VITAGL_LIB ${VITAGL_DIR}/libvitaGL.a)

# Force a full rebuild of vitaGL whenever VITAGL_MAKE_FLAGS change
set(VITAGL_FLAGS_STAMP ${CMAKE_BINARY_DIR}/vitagl_flags.stamp)
set(vitagl_old_flags "")
if (EXISTS ${VITAGL_FLAGS_STAMP})
  file(READ ${VITAGL_FLAGS_STAMP} vitagl_old_flags)
endif()
if (NOT vitagl_old_flags STREQUAL VITAGL_MAKE_FLAGS)
  file(GLOB_RECURSE vitagl_stale_objs ${VITAGL_DIR}/source/*.o)
  file(REMOVE ${vitagl_stale_objs} ${VITAGL_LIB})
  file(WRITE ${VITAGL_FLAGS_STAMP} "${VITAGL_MAKE_FLAGS}")
endif()

add_custom_target(vitaGL_build
                  COMMAND make -C ${VITAGL_DIR} -j4 ${VITAGL_MAKE_ARGS}
                  BYPRODUCTS ${VITAGL_LIB}
                  COMMENT "Building VitaGL"
                  VERBATIM)

add_library(vitaGL STATIC IMPORTED)
set_target_properties(vitaGL PROPERTIES
                      IMPORTED_LOCATION ${VITAGL_LIB}
                      INTERFACE_INCLUDE_DIRECTORIES ${VITAGL_DIR}/source)
add_dependencies(${CMAKE_PROJECT_NAME} vitaGL_build)

add_subdirectory(lib/libc_bridge)
add_dependencies(${CMAKE_PROJECT_NAME} SceLibcBridge)

target_include_directories(${CMAKE_PROJECT_NAME} SYSTEM
                           PUBLIC ${CMAKE_SOURCE_DIR}/lib
                           )

target_include_directories(${CMAKE_PROJECT_NAME}
                           PUBLIC ${CMAKE_SOURCE_DIR}/source
                           )

target_link_libraries(${CMAKE_PROJECT_NAME}
                      -Wl,--whole-archive pthread -Wl,--no-whole-archive
                      stdc++
                      vitaGL
                      vitashark
                      SceShaccCgExt
                      taihen_stub
                      SceShaccCg_stub
                      z
                      m
                      ScePgf_stub
                      mathneon
                      kubridge_stub
                      ${CMAKE_BINARY_DIR}/lib/libc_bridge/stubs/libSceLibcBridge_stub.a
                      SceHid_stub
                      SceTouch_stub
                      SceMotion_stub
                      SceAppMgr_stub
                      SceAppUtil_stub
                      SceAudio_stub
                      SceAudioIn_stub
                      SceCommonDialog_stub
                      SceDisplay_stub
                      SceKernelDmacMgr_stub
                      SceFios2_stub
                      SceCtrl_stub
                      SceGxm_stub
                      ScePower_stub
                      SceTouch_stub
                      SceVshBridge_stub
                      SceNet_stub
                      SceNetCtl_stub
                      SceSysmodule_stub
                      SceRtc_stub
                      SceAvPlayer_stub
                      SceVideodec_stub
                      avcodec
                      avutil
                      swresample
                      mp3lame
                      )

vita_create_self(eboot.bin ${CMAKE_PROJECT_NAME} UNSAFE NOASLR)
vita_create_vpk(${VITA_VPKNAME}.vpk ${VITA_TITLEID} eboot.bin
                VERSION ${VITA_VERSION}
                NAME ${VITA_APP_NAME}
                FILE ${CMAKE_SOURCE_DIR}/extras/cpuinfo cpuinfo
                ${CMAKE_SOURCE_DIR}/extras/meminfo meminfo
                ${CMAKE_SOURCE_DIR}/extras/livearea/icon0.png sce_sys/icon0.png
                ${CMAKE_SOURCE_DIR}/extras/livearea/pic0.png sce_sys/pic0.png
                ${CMAKE_SOURCE_DIR}/extras/livearea/startup.png sce_sys/livearea/contents/startup.png
                ${CMAKE_SOURCE_DIR}/extras/livearea/bg0.png sce_sys/livearea/contents/bg0.png
                ${CMAKE_SOURCE_DIR}/extras/livearea/template.xml sce_sys/livearea/contents/template.xml
                )

add_custom_target(send
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND curl -T eboot.bin ftp://${PSVITAIP}:1337/ux0:/app/${VITA_TITLEID}/
                  COMMAND echo launch ${VITA_TITLEID} | nc ${PSVITAIP} 1338
                  DEPENDS ${VITA_VPKNAME}.vpk-vpk
                  )

add_custom_target(send_kvdb
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND curl -T eboot.bin ftp://${PSVITAIP}:1337/ux0:/app/${VITA_TITLEID}/
                  COMMAND echo launch ux0:/app/${VITA_TITLEID}/eboot.bin | nc ${PSVITAIP} 1338
                  DEPENDS ${VITA_VPKNAME}.vpk-vpk
                  )

add_custom_target(dump
                  COMMAND ${CMAKE_SOURCE_DIR}/extras/scripts/get_dump.sh ${PSVITAIP}
                  COMMAND python3 ${PARSECOREPATH} coredump ${CMAKE_PROJECT_NAME}
                  )

add_custom_target(reboot
                  COMMAND echo reboot | nc ${PSVITAIP} 1338
                  )
