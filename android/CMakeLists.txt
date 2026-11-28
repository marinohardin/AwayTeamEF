cmake_minimum_required(VERSION 3.21)

# ---------------------------------------------------------------------------
# AwayTeamEF for Android (arm64-v8a): the native half of the APK.
#
#   libSDL3.so      SDL 3 (the platform layer; its Java glue is org.libsdl.app.SDLActivity)
#   libSDL2.so      sdl2-compat on SDL 3, the same pair the desktop builds use
#   libmain.so      the engine (single player + Holomatch), renderervk linked in;
#                   SDLActivity calls its SDL_main()
#   libefgame.so    the single-player game + cgame (efgame/), dlopen'd by the SP bridge
#   libefui.so      the single-player ui (efgame/)
#   (EF_VR=ON, the Quest build: the vr module inside libmain.so; make-apk.sh adds the
#    Khronos libopenxr_loader.so beside it)
#
# Driven by android/make-apk.sh (no Gradle). Configure by hand with:
#   cmake -S android -B build-android/native -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
#     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_shared \
#     -DEF_SDL3_SOURCE=<SDL3-x.y.z> -DEF_SDL2COMPAT_SOURCE=<sdl2-compat-x.y.z>
#
# Desktop builds never read this file: they use the top-level CMakeLists.txt and
# EFAndroid-SP/app/jni/engine/CMakeLists.txt. The source lists below mirror the
# engine's; keep them in step when files are added there.
# ---------------------------------------------------------------------------

project(AwayTeamEF_Android C CXX)

if(NOT ANDROID OR NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "android/CMakeLists.txt is for the NDK toolchain with ANDROID_ABI=arm64-v8a")
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/EFPlatform.cmake)

set(EF_ANDROID_PACKAGE "org.awayteamef.app" CACHE STRING "Android application id (also names the data folder)")
# the app version: the desktop builds' (EFAndroid-SP/app/jni/engine/CMakeLists.txt), unless given
# (make-apk.sh passes its VERSION, which defaults to the same)
file(STRINGS ${EF_ROOT}/EFAndroid-SP/app/jni/engine/CMakeLists.txt _ef_version_line
  REGEX "^set\\(EF_APP_VERSION \"[^\"]+\"")
string(REGEX REPLACE "^set\\(EF_APP_VERSION \"([^\"]+)\".*" "\\1" _ef_version "${_ef_version_line}")
set(EF_APP_VERSION "${_ef_version}" CACHE STRING "App version shown in the console and to server browsers")
set(EF_SDL3_SOURCE "" CACHE PATH "Unpacked SDL3 source (deps/versions.sh SDL3_VERSION)")
set(EF_SDL2COMPAT_SOURCE "" CACHE PATH "Unpacked sdl2-compat source (deps/versions.sh SDL2COMPAT_VERSION)")
# Bink cinematics need FFmpeg; the Android build has none yet, so movies are skipped
# (bink_stub.c), as on a desktop build with EF_BINK=OFF.
option(EF_BINK "Bink cinematics via FFmpeg (needs an Android FFmpeg; not wired up yet)" OFF)
option(EF_VM_JIT "QVM JIT (vm_aarch64.c), as the old Android build used" ON)
# The Quest build (make-apk.sh VR=1): OpenXR rendering and controllers (efcode/vr, the
# renderer's EF_VR hooks) plus the shell's JNI hook (android/jni/quest_jni.c). OFF = the flat APK.
option(EF_VR "Quest VR build: OpenXR (efcode/vr) and the renderer's VR hooks" OFF)

if(NOT EXISTS "${EF_SDL3_SOURCE}/CMakeLists.txt" OR NOT EXISTS "${EF_SDL2COMPAT_SOURCE}/CMakeLists.txt")
  message(FATAL_ERROR "Set EF_SDL3_SOURCE and EF_SDL2COMPAT_SOURCE (android/make-apk.sh unpacks them)")
endif()
if(EF_BINK)
  message(FATAL_ERROR "EF_BINK: there is no Android FFmpeg build yet")
endif()

# 16 KB page alignment for every library (Android 15+ devices; harmless elsewhere)
add_link_options(-Wl,-z,max-page-size=16384)
# keep the host's build paths out of the libraries
add_compile_options(-ffile-prefix-map=${EF_ROOT}=/src -fmacro-prefix-map=${EF_ROOT}=/src)

# ---- SDL 3 + sdl2-compat ----
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${EF_SDL3_SOURCE} sdl3 EXCLUDE_FROM_ALL)
set(SDL2COMPAT_TESTS OFF CACHE BOOL "" FORCE)
set(SDL2COMPAT_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL2COMPAT_STATIC OFF CACHE BOOL "" FORCE)
set(SDL2COMPAT_X11 OFF CACHE BOOL "" FORCE)
add_subdirectory(${EF_SDL2COMPAT_SOURCE} sdl2compat EXCLUDE_FROM_ALL)
# sdl2-compat finds SDL 3 with dlopen("libSDL3.so") at run time; the APK carries both

# ---- the game modules (efgame/: libefgame.so, libefui.so) ----
# Co-op (host and join) in the Quest build, where the start screen has buttons for it
# (android/java-quest StartActivity); the flat phone build has no co-op yet. The game's co-op
# code is the same as on Linux (efgame/src/coop); the engine's side is sys_coop_host.c, below.
set(EF_COOP ${EF_VR} CACHE BOOL "" FORCE)
add_subdirectory(${EF_ROOT}/efgame efgame)

# ---- the engine: libmain.so ----
set(JNI ${EF_ROOT}/EFAndroid-SP/app/jni)
set(EF  ${JNI}/efcode)
set(SP  ${JNI}/sp)
set(EFG ${EF_ROOT}/efgame)
if(EXISTS ${JNI}/compat/bink_stub.c)
  set(EF_BINK_STUB ${JNI}/compat/bink_stub.c)
else()
  set(EF_BINK_STUB ${JNI}/macos/bink_stub.c)
endif()

set(CLIENT_SRC
  ${EF}/client/cl_cgame.c   ${EF}/client/cl_cin.c     ${EF}/client/cl_console.c
  ${EF}/client/cl_input.c   ${EF}/client/cl_keys.c    ${EF}/client/cl_main.c
  ${EF}/client/cl_net_chan.c ${EF}/client/cl_parse.c  ${EF}/client/cl_scrn.c
  ${EF}/client/cl_ui.c      ${EF}/client/cl_avi.c
  ${EF}/client/snd_adpcm.c  ${EF}/client/snd_dma.c    ${EF}/client/snd_mem.c
  ${EF}/client/snd_mix.c    ${EF}/client/snd_wavelet.c
  ${EF}/client/snd_main.c   ${EF}/client/snd_codec.c  ${EF}/client/snd_codec_wav.c
  ${EF}/client/snd_codec_mp3.c
  ${EF}/client/qal.c        ${EF}/client/snd_openal.c)
file(GLOB LIBMAD_SRC ${EF}/libmad-0.15.1b/*.c)
set(QCOMMON_SRC
  ${EF}/qcommon/cm_load.c   ${EF}/qcommon/cm_patch.c  ${EF}/qcommon/cm_polylib.c
  ${EF}/qcommon/cm_test.c   ${EF}/qcommon/cm_trace.c
  ${EF}/qcommon/cmd.c       ${EF}/qcommon/common.c    ${EF}/qcommon/cvar.c
  ${EF}/qcommon/files.c     ${EF}/qcommon/md4.c       ${EF}/qcommon/md5.c
  ${EF}/qcommon/cfg_overlay.c
  ${EF}/qcommon/msg.c       ${EF}/qcommon/net_chan.c  ${EF}/qcommon/net_ip.c
  ${EF}/qcommon/huffman.c   ${EF}/qcommon/q_math.c    ${EF}/qcommon/q_shared.c
  ${EF}/qcommon/unzip.c     ${EF}/qcommon/ioapi.c     ${EF}/qcommon/puff.c
  ${EF}/qcommon/vm.c        ${EF}/qcommon/vm_interpreted.c
  ${EF}/qcommon/regress.c)
if(EF_VM_JIT)
  list(APPEND QCOMMON_SRC ${EF}/qcommon/vm_aarch64.c)
endif()
set(SERVER_SRC
  ${EF}/server/sv_bot.c     ${EF}/server/sv_ccmds.c   ${EF}/server/sv_client.c
  ${EF}/server/sv_game.c    ${EF}/server/sv_init.c    ${EF}/server/sv_main.c
  ${EF}/server/sv_net_chan.c ${EF}/server/sv_snapshot.c ${EF}/server/sv_world.c)
file(GLOB BOTLIB_SRC ${EF}/botlib/*.c)
set(SDL_SRC ${EF}/sdl/sdl_input.c ${EF}/sdl/sdl_snd.c ${EF}/sdl/sdl_gamma.c ${EF}/sdl/sdl_glimp.c)
# Android's sys layer: sys_unix.c (paths, dialogs via SDL), con_passive.c (no tty);
# sys_platform.h gives Android the no-op launcher/data hooks
set(SYS_MAIN ${EF}/sys/sys_main.c)
set(SYS_SRC ${EF}/sys/con_log.c ${EF}/sys/con_passive.c ${EF}/sys/sys_autoupdater.c
  ${EF}/sys/sys_unix.c ${SYS_MAIN})
file(GLOB RENDERCOMMON_SRC ${EF}/renderercommon/*.c)
file(GLOB RENDERER_SRC ${EF}/renderervk/*.c)
list(REMOVE_ITEM RENDERER_SRC ${EF}/renderervk/tr_subs.c)
list(REMOVE_ITEM RENDERER_SRC ${EF}/renderervk/shaders/spirv/shader_data.c)
file(GLOB ZLIB_SRC ${EF}/zlib/*.c)
file(GLOB JPEG_SRC ${EF}/jpeg-8c/*.c)

set(EF_ENGINE_COPTS -fno-strict-aliasing -Werror=implicit-function-declaration -O2 -w
  -Wno-error=incompatible-function-pointer-types -Wno-error=int-conversion)

# SP game/cgame bridge (C++ against the SP game headers)
add_library(sp_game_bridge STATIC ${SP}/sp_bridge.cpp ${SP}/sp_ui_bridge.cpp ${SP}/sp_clink.c)
target_include_directories(sp_game_bridge PRIVATE
  ${EFG}/android ${EFG}/src/game ${EFG}/src/cgame ${EFG}/src/ui
  ${EFG}/src/client ${EFG}/src/renderer ${EFG}/src/qcommon ${SP})
target_compile_options(sp_game_bridge PRIVATE
  -include ${EFG}/android/ef_android_compat.h -fno-threadsafe-statics -fcommon -w)
target_compile_definitions(sp_game_bridge PRIVATE NDEBUG EF_DYLIB_MODULES
  $<$<BOOL:${EF_COOP}>:EF_COOP>)
set_property(TARGET sp_game_bridge PROPERTY CXX_STANDARD 17)

if(EF_COOP)
  # engine side of co-op, as on Linux (engine/CMakeLists.txt): sys_coop_net.c compiles unchanged
  # with sys_coop_compat.h forced in (raw deflate through zlib, arc4random), and zlib's deflate
  list(APPEND SYS_SRC ${EF}/sys/sys_coop_host.c ${EF}/sys/sys_coop_net.c
    ${EF}/zlib-deflate/deflate.c ${EF}/zlib-deflate/trees.c ${EF}/zlib-deflate/compress.c)
  set_source_files_properties(${EF}/sys/sys_coop_net.c PROPERTIES
    COMPILE_OPTIONS "-include;${EF}/sys/sys_coop_compat.h"
    INCLUDE_DIRECTORIES "${EF}/sys/coop_compat")
  set_source_files_properties(${EF}/sys/sys_coop_host.c PROPERTIES
    INCLUDE_DIRECTORIES "${EF}/sys/coop_compat")
endif()

add_library(main SHARED
  ${CLIENT_SRC} ${QCOMMON_SRC} ${SERVER_SRC} ${BOTLIB_SRC}
  ${SDL_SRC} ${SYS_SRC}
  ${RENDERER_SRC} ${RENDERCOMMON_SRC}
  ${ZLIB_SRC} ${JPEG_SRC} ${LIBMAD_SRC}
  ${SP}/sp_integration.c ${EF_BINK_STUB})
# SDLActivity looks up SDL_main() in libmain.so
set_source_files_properties(${SYS_MAIN} PROPERTIES COMPILE_DEFINITIONS "main=SDL_main")

target_include_directories(main PRIVATE
  ${EF_SDL2COMPAT_SOURCE}/include/SDL2
  ${EF} ${EF}/zlib ${EF}/jpeg-8c ${EF}/AL
  ${EF}/libmad-0.15.1b ${EF}/libmad-0.15.1b/include
  ${SP})
target_compile_definitions(main PRIVATE
  ELITEFORCE BOTLIB NDEBUG
  USE_CODEC_MP3 USE_INTERNAL_MP3 FPM_64BIT
  $<$<NOT:$<BOOL:${EF_VM_JIT}>>:NO_VM_COMPILED>
  USE_VULKAN USE_VULKAN_API
  USE_FLEXIBLE_DISPLAY=1
  USE_OPENAL USE_OPENAL_DLOPEN
  PRODUCT_VERSION="1.40"
  EF_APP_VERSION="${EF_APP_VERSION}"
  $<$<BOOL:${EF_COOP}>:EF_COOP>
  ARCH_STRING="arm64"
  USE_LOCAL_HEADERS=1
  EF_HOMEPATH_NAME="AwayTeamEF"
  # the app's own OBB folder: readable and writable by the app with no storage permission,
  # and reachable with adb push. Game data goes in <this>/baseEF/.
  ANDROID_DATA_ROOT="/sdcard/Android/obb/${EF_ANDROID_PACKAGE}")
target_compile_options(main PRIVATE ${EF_ENGINE_COPTS})
target_link_libraries(main PRIVATE sp_game_bridge SDL2 android log dl m)
if(EF_VR)
  add_subdirectory(${EF}/vr ef_vr)
  # ef_vr defines EF_VR for the engine and brings vr_input_engine.c along (an INTERFACE source);
  # QuestActivity hands the JavaVM and activity to the vr module through quest_jni.c.
  # libopenxr_loader.so is not linked: vr_openxr.c opens it at run time (make-apk.sh builds it).
  target_sources(main PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/jni/quest_jni.c)
  target_link_libraries(main PRIVATE ef_vr)
  # EF_QUEST (q_shared.h): the desktop blocks with a Quest counterpart: "AwayTeamEF for Meta Quest
  # <version>", the menus' Quit back to the start screen, the Loopback Host Fix, cMod's paks
  target_compile_definitions(main PRIVATE EF_QUEST)
  # the launchers' core for the Loopback Host Fix's public address (lc_nat.c: NAT-PMP, UPnP); only
  # what quest_jni.c uses is linked. No GOG unpacking (no liblzma here).
  set(LC_GOG OFF CACHE BOOL "" FORCE)
  set(LC_BUNDLE_ZLIB OFF CACHE BOOL "" FORCE)
  add_subdirectory(${EF}/launcher eflauncher)
  # -U_FORTIFY_SOURCE: bionic's realpath() check trips on a launcher path buffer quest_jni.c never uses
  target_compile_options(eflauncher PRIVATE -w -U_FORTIFY_SOURCE)
  target_link_libraries(main PRIVATE eflauncher)
  # the SP bridge's VR hooks: room-scale movement (vr_move.h) and the hand-pose syscall
  # CG_VR_GETPOSE (vr_input.h)
  target_compile_definitions(sp_game_bridge PRIVATE EF_VR)
  target_include_directories(sp_game_bridge PRIVATE ${EF}/vr)
endif()
add_dependencies(main SDL3-shared efgame efui)
