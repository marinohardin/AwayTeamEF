# Quest VR: rendering design

Status: spike 1 (2026-11-26). The OpenXR plumbing and the renderer hooks are in the tree behind
the CMake option `EF_VR` (off by default). It compiles for macOS and Android arm64. It has not yet run
against an OpenXR runtime: there is no headset yet, and the Meta XR Simulator isn't installed.
See [PLAN.md](PLAN.md) for the goal and the phases.

## Where the code is

| Piece | Path | What it does |
|---|---|---|
| OpenXR headers | `deps/openxr/include/openxr` | Khronos OpenXR-SDK `release-1.1.65`, Apache-2.0 (`deps/openxr/LICENSE`). Headers only |
| OpenXR module | `EFAndroid-SP/app/jni/efcode/vr/` (`vr.h`, `vr_openxr.c`, `CMakeLists.txt`) | Opens the loader at runtime, then runs the instance, system, session, space, swapchain and frame loop. Knows nothing about the game |
| Renderer glue | `renderervk/tr_vr.c` | cvars, the per-frame calls, per-eye view setup |
| Renderer hooks | `renderervk/vk.c`, `tr_main.c`, `tr_scene.c`, `tr_cmds.c`, `tr_init.c`, `tr_local.h`, `vk.h` | All inside `#ifdef EF_VR` |
| Build switch | `option(EF_VR ...)` in `EFAndroid-SP/app/jni/engine/CMakeLists.txt` | Adds `efcode/vr` as the `ef_vr` library. Linking it defines `EF_VR` for the engine |

Without `EF_VR`, every changed renderer file preprocesses to exactly the same translation unit as
before, and `tr_vr.c` preprocesses to nothing. The desktop releases are therefore unchanged.

## Startup: OpenXR owns the instance and the device

Under `XR_KHR_vulkan_enable2`, the runtime creates the Vulkan objects. The renderer hands over its
normal create-info structs:

1. `VR_Init`. Finds the loader with `dlopen`: `libopenxr_loader.so` on Android, or the path in
   `EF_OPENXR_LOADER`. Then it calls `xrCreateInstance` (on Android, `xrInitializeLoaderKHR` comes first), `xrGetSystem` for the
   HMD form factor, the stereo view configuration, and `xrGetVulkanGraphicsRequirements2KHR`. If
   any step fails, the game stays flat.
2. `create_instance`. Calls `VR_CreateVulkanInstance` in place of `vkCreateInstance`. The API version is raised
   to at least 1.1 and to the runtime's minimum.
3. GPU choice. Uses `xrGetVulkanGraphicsDevice2KHR`; `r_device` is ignored in VR.
4. `vk_create_device`. Calls `VR_CreateVulkanDevice` in place of `vkCreateDevice`.
5. `vk_initialize`. Calls `VR_CreateSession` with the device's graphics queue, then the reference space
   (STAGE, with LOCAL as the fallback) and one swapchain per eye at the recommended size.
6. If any later step fails, VR is switched off and the game carries on flat with the
   instance and device it already has.

Teardown runs in reverse: the session and its swapchains go before `vkDestroyDevice`, and the XR instance goes before
`vkDestroyInstance`. A `vid_restart` that keeps the instance rebuilds only the device and
the session.

The runtime cvar `vr_enable` (latched) defaults to 1 in the Android build and to 0 on the desktop. So
a desktop `EF_VR` build still renders flat unless someone asks for VR. `r_smp 1` disables VR for
now, because the eye images must be released after the frame has been submitted on the same thread.

## Frames and stereo

- `RE_BeginFrame` calls `VR_BeginFrame`. That polls the events (the session starts on READY), runs
  `xrWaitFrame` and `xrBeginFrame`, gets both eye poses and FOVs from `xrLocateViews`, and acquires and waits
  on both eye images.
- `RE_RenderScene` draws a world scene (any scene without `RDF_NOWORLDMODEL`) once per eye
  (`R_VR_SetupEye`):
  - **Viewport:** the eye's half of the game's viewport, left eye on the left.
  - **Orientation:** the body yaw comes from the game's view. Pitch and roll come only from the headset.
    The engine converts OpenXR axes (+X right, +Y up, −Z forward) to the game's axes
    (+X forward, +Y left, +Z up).
  - **Origin:** the game's eye position plus this eye's offset from the recentred head origin,
    multiplied by `vr_worldScale` game units per metre. `vr_recenter` resets the origin, and it is set
    automatically on the first tracked frame.
  - **Projection:** taken straight from the eye's `XrFovf` tangents (`R_VR_SetupProjection` in
    `tr_main.c`). It is a true asymmetric frustum with matching cull planes. The game's `fov_x`/`fov_y`
    and `r_stereoSeparation` aren't used.
  - Both eyes share one draw-surface list, the same way portals do. Culling still runs per eye.
- At the end of the frame, `vk_end_frame` blits each half of the window image into its eye's
  swapchain image (`vk_vr_copy_eyes`). Then `RE_EndFrame` calls `VR_EndFrame`, which releases the
  images and calls `xrEndFrame` with one projection layer. If the renderer dropped the frame (minimized, or a
  geometry-buffer resize), it submits no layers.
- The desktop window keeps presenting, so it doubles as a mirror.

**Why blit first?** It needs no new render passes or pipelines, it handles any eye size and format, and
it proves the whole OpenXR round trip. Its costs: one full-screen copy per eye, eye
resolution bounded by the window, and a second present that the Quest doesn't need. The next step
renders straight into the eye images:

1. Wrap each eye swapchain image in a framebuffer that is compatible with the main render pass
   (`vk.render_pass.main`). For MSAA, also an eye-sized resolve target.
2. Run the 3D pass once per eye into that framebuffer, with the full eye viewport (no halves).
3. Drop the window swapchain on the Quest. No `vkAcquireNextImageKHR`/present happens in VR; the frame pacing
   comes from `xrWaitFrame` alone.

## Colour space

The game's colours are gamma-encoded. The eye swapchain prefers `R8G8B8A8_UNORM`/`B8G8R8A8_UNORM`, so
the blit keeps the values exactly. Runtimes treat UNORM images as linear, though, so on the headset the image may look washed out. That has to be checked on a
device. There are two fixes, both small:

- **sRGB swapchain, with linear output from the gamma pass.** With `r_fbo`, the gamma post-pass
  shader writes `pow(c, 2.2)`. The blit into an sRGB image then re-encodes it correctly.
- **sRGB swapchain, written through a UNORM view.** Create the swapchain with
  `XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT` and render through a UNORM image view. This fits the
  direct-render step above.

## HUD, menus and 2D (plan; stub today)

Today the 2D draws still cover the whole window, so the HUD is split across the two halves. That's
acceptable for the spike only. The plan is to send every 2D draw (HUD, menus, console, loading screens) into an
offscreen 640×480-aspect image and show it as an OpenXR layer:

- **2D target:** in VR, `RB_SetGL2D` and the `RE_StretchPic` family render into a separate colour
  image of their own (about 1024×768), not into the eye frame. The 3D scenes that menus use
  (`RDF_NOWORLDMODEL`: player models, HUD heads) are drawn mono into the same image.
- **Layer:** an `XrCompositionLayerQuad`, submitted after the projection layer. Later, possibly
  `XR_KHR_composition_layer_cylinder`.
  - **Menus:** fixed in the world, about 2 m away and about 1.6 m wide, re-placed in front of the player when a menu
    opens.
  - **HUD:** fixed to the body at about 1.2 m, slightly below eye level, so it doesn't swim with every head turn.
- **Pointer:** a controller ray hits the quad and gives 640×480 coordinates. Those go to the UI as mouse
  positions (the SP UI) or as relative deltas (the Holomatch UI).
- **Crosshair:** in SP, a 3D crosshair or laser on the hand ray, not the 2D one. For Holomatch, see PLAN.md (H1).

## Cutscenes: virtual screen

- **Scripted cameras:** the cgame already publishes `cg_cameraActive`. While it is set, VR draws the
  camera's view mono into an offscreen image and shows it on a quad about 3.5 m away (theater mode).
  The head stays free, and the world camera never drives the head.
- **Bink FMV:** `SCR_DrawCinematic` uses the same screen.
- **Forced view angles** outside a camera (scripted turns, knockback) rotate the body yaw only. Screen
  shake is scaled down or removed in VR.

## World scale

- `vr_worldScale` (default 32 game units per metre) scales the head offset.
- The game's eye height is about 56 units for a standing player, about 1.75 m at that scale.
- STAGE space gives the real floor height. The first version uses only the position relative to the
  recentred head, so the game's own eye height stays authoritative, and crouching and leaning work by
  moving the real head.
- Seated play and height calibration come with the comfort options (phase 2).
- Tune the default on the headset. 32 is a first guess: doors and corridors should feel right.

## Quest 2 performance plan (72 Hz)

At 72 Hz, the budget is about 13.9 ms for both eyes. The Quest 2's default eye buffer is about 1440×1540; the session uses whatever size the runtime recommends.

1. **Measure first.** Run the flat APK on the headset (phase 1), then this spike's stereo path, and read the
   GPU and CPU frame times with `xrWaitFrame` pacing.
2. **Render straight to the eye images**, with no window swapchain and no blits (see above).
3. **Skip the full-screen post passes in VR:** bloom and the gamma pass (the `r_fbo` path). On a tiled GPU, each
   full-screen pass costs bandwidth.
4. **Fixed foveation:** `XR_FB_foveation` plus `XR_FB_foveation_vulkan` (a fragment-density-map
   image per swapchain), with level and dynamic settings from `XR_FB_foveation_configuration`.
5. **Refresh rate:** 72 Hz first. `XR_FB_display_refresh_rate` exposes 80/90 Hz as an option once there is headroom.
6. **Resolution scale:** a VR cvar on top of the recommended eye size, for example 0.9 to 1.2.
7. **Multiview later:** only if profiling shows the CPU recording both eyes is the limit.
   - Option A: `VK_KHR_multiview` with one array swapchain, a two-layer render pass and
     per-view matrices in the shaders.
   - Option B: record the shared front end once and replay the back end per eye.

   Both touch every pipeline, so they wait for data.

## Open questions for the headset

- Does the Quest runtime accept the renderer's device features and extensions as they are? The
  device-create call goes through the runtime, which adds its own.
- Swapchain format and sRGB behaviour (see Colour space).
- Does the Android shell keep the window swapchain for the first test? Easiest is to keep it
  as a mirror.
- Which loader the APK carries: the Khronos loader `libopenxr_loader.so` from the Khronos release AAR.
  Meta documents that the Khronos loader 1.0.34 or newer works on Quest OS v62 or newer.
