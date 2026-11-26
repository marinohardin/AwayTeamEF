# ---------------------------------------------------------------------------
# ef_vr: the OpenXR side of the VR build (vr.h, vr_openxr.c). Only added when EF_VR is ON
# (engine/CMakeLists.txt); linking it also defines EF_VR for the target, which turns on the
# renderer's VR hooks (renderervk: vk.c, tr_vr.c, tr_main.c, tr_scene.c, tr_cmds.c).
#
# Headers only: the Khronos OpenXR headers in deps/openxr/include. The OpenXR loader is
# opened at runtime (dlopen), so nothing extra is linked; on the Quest the APK carries
# libopenxr_loader.so.
# ---------------------------------------------------------------------------

set(EF_OPENXR_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/../../../../../deps/openxr/include"
  CACHE PATH "Khronos OpenXR headers (openxr/openxr.h)")

add_library(ef_vr STATIC ${CMAKE_CURRENT_LIST_DIR}/vr_openxr.c)
target_include_directories(ef_vr PUBLIC ${CMAKE_CURRENT_LIST_DIR} ${EF_OPENXR_INCLUDE})
target_compile_definitions(ef_vr PUBLIC EF_VR)
target_link_libraries(ef_vr PRIVATE ${CMAKE_DL_LIBS})
