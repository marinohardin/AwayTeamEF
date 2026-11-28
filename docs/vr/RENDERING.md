# Quest VR: rendering design

Status: phase 2, eye rendering (2026-11-26). Everything is behind the CMake option `EF_VR` (off by
default) and the latched cvar `vr_enable`. The world renders straight into the OpenXR eye images, and
the 2D pass is a quad layer. It runs in the Meta XR Simulator on macOS (Quest 3 profile) at the
72 Hz display rate; it has not yet run on a headset. See [PLAN.md](PLAN.md) for the goal and the phases,
and [QUEST.md](QUEST.md) for the APK and controller input.

## Where the code is

| Piece | Path | What it does |
|---|---|---|
| OpenXR headers | `deps/openxr/include/openxr` | Khronos OpenXR-SDK `release-1.1.65`, Apache-2.0 (`deps/openxr/LICENSE`). Headers only |
| OpenXR module | `EFAndroid-SP/app/jni/efcode/vr/` (`vr.h`, `vr_openxr.c`, `CMakeLists.txt`) | Opens the loader at runtime, then runs the instance, system, session, space, swapchain and frame loop. Knows nothing about the game |
| Renderer glue | `renderervk/tr_vr.c` | cvars, the per-frame calls, per-eye view setup |
| Renderer hooks | `renderervk/vk.c`, `tr_main.c`, `tr_scene.c`, `tr_cmds.c`, `tr_backend.c`, `tr_image.c`, `tr_init.c`, `tr_local.h`, `vk.h` | All inside `#ifdef EF_VR` |
| UI state | `client/cl_scrn.c` | Sets the read-only cvar `vr_uiState` each frame (menu up, cinematic playing). No game-module change |
| Desktop loader | `macos/build-openxr-loader.sh` | Builds the Khronos loader as a dylib for simulator tests |
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
5. `vk_initialize`. Hands the cvars to the module (`VR_SetOptions`), then calls `VR_CreateSession`
   with the device's graphics queue. That creates the reference space (STAGE, with LOCAL as the
   fallback) plus a VIEW space, one swapchain per eye, the dimmer swapchain, and asks for
   `vr_refreshRate` through `XR_FB_display_refresh_rate` (the closest offered rate). Once the
   framebuffers exist, the renderer creates the 2D layer swapchain at the window size (`VR_SetCanvas`).
6. If any later step fails, VR is switched off and the game carries on flat with the
   instance and device it already has.

Teardown (quit, `vid_restart`) runs in this order, all while the Vulkan device and instance are alive:

1. `vkDeviceWaitIdle`, so nothing still uses the eye images.
2. The session is ended the OpenXR way: `xrRequestExitSession`, empty frames until the runtime
   reports STOPPING, `xrEndSession`, then a short wait for IDLE and EXITING. Both waits are bounded
   (2 s and 0.5 s), so a runtime that doesn't answer can't hang the quit.
3. `xrDestroySwapchain` for every swapchain, `xrDestroySpace`, `xrDestroySession`, the action set, and
   `xrDestroyInstance`.
4. Only then `vkDestroyDevice` and `vkDestroyInstance`, which OpenXR created.

The Meta XR Simulator keeps its own wrappers around our device and queue alive until the session's
exit has reached its frontend; destroying the device first made it crash on quit.
A map change keeps the device, the instance and the session (the renderer
restarts with the context kept).

**The runtime ends the session.** If the session reaches EXITING or LOSS_PENDING without our own
exit request (the user quit from the headset's system menu, or the headset was lost), VR is off for
the rest of the run: the Quest build quits, and a desktop build carries on in the flat window (the
renderer already draws flat while no session runs; at the main menu it also does a `vid_restart`,
which comes back flat). The command `vr_debugEndSession` asks the runtime to end the session, to test
this path.

**Android pause and resume** (code review; no headset yet). The Quest menu button only drops the
session from FOCUSED to VISIBLE: frames continue and the controls are released. Taking the headset
off sends STOPPING, then IDLE, and pauses the activity. SDL blocks the game thread while the activity
is paused, so the events are handled on resume, in order: `xrEndSession` at STOPPING, then
`xrBeginSession` at READY. Quitting from the system menu ends in EXITING, which quits the game.

The runtime cvar `vr_enable` (latched) defaults to 1 in the Android build and to 0 on the desktop. So
a desktop `EF_VR` build still renders flat unless someone asks for VR. `r_smp 1` disables VR for
now, because the eye images must be released after the frame has been submitted on the same thread.

## Frames and stereo

Each frame is one of three kinds (`R_VR_FrameKind` in `tr_vr.c`):

| Kind | When | Eyes | 2D quad |
|---|---|---|---|
| Game | otherwise | fresh, both eyes | HUD: lazy follow, translucent |
| Menu | `vr_uiState` 1 (a UI has the keys, or loading) | frozen, dimmed | menu: soft follow (or `vr_menuFollow 0`, `vr_menuHeadLocked`), translucent |
| Theater | `cg_cameraActive`, or `vr_uiState` 2 (Bink or in-game cinematic) | frozen, dimmed | big screen, opaque |

- `RE_BeginFrame` calls `VR_BeginFrame( wantEyes, dim )`. That polls the events, runs
  `xrWaitFrame`/`xrBeginFrame`, locates both views, and acquires the 2D image every frame. The eye
  images are acquired only for a Game frame; the dimmer image only when its alpha changes.
- `RE_RenderScene` draws a world scene (no `RDF_NOWORLDMODEL`) once per eye in a Game frame
  (`R_VR_SetupEye`), not at all in a Menu frame, and once, flat, into the 2D image in a Theater frame:
  - **Viewport:** the whole eye image.
  - **Orientation:** the body yaw comes from the game's view; pitch and roll come only from the headset.
    OpenXR axes (+X right, +Y up, −Z forward) become the game's (+X forward, +Y left, +Z up).
  - **Origin:** the game's eye plus this eye's offset from the recentred head origin, times
    `vr_worldScale` game units per metre. The eye separation is the runtime's (its IPD). `vr_recenter`
    resets the origin; it is set automatically on the first tracked frame and on a reference-space change.
  - **Projection:** straight from the eye's `XrFovf` tangents (`R_VR_SetupProjection` in `tr_main.c`),
    a true asymmetric frustum with matching cull planes. `fov_x`/`fov_y` and `r_stereoSeparation`
    aren't used. On the simulator's Quest 3 profile the left eye is 54° left / 40° right and the right
    eye the mirror, so the two eye images are framed about a quarter of their width apart; that is
    expected.
  - Both eyes share one draw-surface list, the same way portals do. Culling runs per eye.
- `VR_EndFrame` releases the images and submits, in order: the projection layer (fresh views, or the
  last fresh eye images with their old poses when frozen), the dimmer quad when dimming, then the 2D
  quad. If the renderer dropped the frame it submits no layers.
- The desktop window keeps presenting the 2D pass, so it doubles as a mirror of the menus and HUD.

## Rendering into the eye images

- **Render pass.** `RENDER_PASS_VR_EYE` (`vk.h`) is an extra render-pass slot, so pipelines are created
  lazily for it like for the other passes. Its attachments: the eye image view (colour, or MSAA resolve
  target), an eye-sized depth buffer, and with `vr_msaa` > 1 an eye-sized multisample colour image.
  The two eyes share the depth and MSAA images; the passes run one after the other.
- **Switching.** The backend calls `vk_vr_set_target( eye )` from `RB_DrawSurfs` (the view's `vrEye`) and
  `vk_vr_set_target( -1 )` from `RB_SetGL2D`. The 2D target is the main pass's colour image, resumed by
  a single-sample `main_resume` pass that loads it. `switchRenderPass` (the screen-map pass) is skipped
  in VR.
- **No full-screen post passes.** In VR: bloom off, `r_hdr 0`, screen map off, MSAA off on the main
  (2D) pass, the gamma post-pass skipped for the eyes. `r_fbo 1` is forced, because the 2D image is the
  main pass's colour image.
- **Overbright.** The usual hardware-gamma overbright can't apply to eye images, so in VR the overbright
  bits are folded into the lightmaps (`tr.overbrightBits = 0`, `R_SetColorMappings`).
- **Desktop window.** It still presents the main image (the 2D pass) as a mirror. On the Quest the window
  surface is a cheap 2D target; dropping it entirely is a later optimisation.

## Colour space

The game's colours are gamma-encoded. Runtimes offer sRGB swapchains (the simulator offers only sRGB),
and an sRGB image means the compositor decodes it. So:

- Each eye swapchain is `R8G8B8A8_SRGB` (then `B8G8R8A8_SRGB`, then UNORM as a fallback), created with
  `XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT`, and rendered through a UNORM view. The gamma-encoded values
  land in the image unchanged and the compositor decodes them correctly.
- The 2D layer swapchain uses the SRGB twin of the main colour format. `vk_vr_end_frame` copies the main
  colour image into it with `vkCmdCopyImage` (same size, compatible formats, raw bits).

## The 2D layer (HUD, menus, console)

- **Coverage.** In VR the main pass clears to alpha 0, and its blend states are adjusted so alpha means
  coverage: a source-alpha blend writes alpha with (ONE, ONE_MINUS_SRC_ALPHA); an additive blend leaves
  alpha alone (ZERO, ONE). The quad is then composited with
  `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT` over the world, so the HUD floats and the rest is
  see-through. Opaque 2D (menu backgrounds) covers the quad. This is an approximation: additive-only
  2D (some glows) can be invisible where nothing else covers it.
- **Size.** The quad is `width` metres wide; its height follows the 2D image's aspect.
- **HUD** (`vr_hudDistance` 1.2 m, `vr_hudWidth` 1.2 m, `vr_hudHeight` −0.1 m, `vr_hudFollow` 2,
  `vr_hudFollowAngle` 30°): 0 = world-locked where it was placed, 1 = head-locked, 2 = lazy follow, the
  default. Lazy follow keeps the HUD level and at the head's position; it turns only once the head
  looks more than `vr_hudFollowAngle` away, then eases back (about 8 % of the gap per frame) until
  within 2°. That keeps it readable without being glued to the eyes.
- **Menus** (`vr_menuDistance` 0.7 m, `vr_menuWidth` 0.9 m, `vr_menuDim` 0.6, `vr_menuFollow` 1,
  `vr_menuFollowLag` 0.25 s, `vr_menuHeadLocked` 0): placed in front of the head when the menu opens, then
  a soft follow: every frame the quad eases toward the head's yaw and pitch (an exponential approach by
  the display time that went by: about 63 % of the gap in `vr_menuFollowLag` seconds, so it is the same at
  any frame rate), with a 2° band that absorbs head jitter, and it keeps to the head's position. It stays
  upright (no roll) and is tilted to face a head that looks up or down (pitch limited to 60°). The pointer
  intersects the quad as submitted, tilt included (`VR_GetLastQuadPitch`). `vr_menuFollow 0` keeps the
  menu where it opened; `vr_menuHeadLocked 1` locks it to the head (no laser then). The world behind is
  frozen (the last eye images, resubmitted with their own poses, so it stays put when the head turns)
  and dimmed. The distances were 1.8 m (HUD) and 2 m (menu) at first, then 1.2 m and 1 m (round 3, K10),
  and the menu is at about arm's length since round 4. They are archived cvars, so `vr_layoutVersion`
  (now 2) resets a saved config to the new numbers once.
  - **Opening from a running game** holds the first `VR_MENU_SETTLE` (3) frames: the world stays frozen, the
    2D layer re-shows its last image (a frame that doesn't acquire a new canvas image: the runtime shows the
    last released one, as it does for the frozen eyes) and nothing is dimmed, so the single-player menu's
    first two frames (an opaque black one, then an empty one) are never shown.
  - **The save thumbnail.** The single-player bridge takes it on the frame the game pauses for the menu: it
    hides the menu for that frame, and the thumbnail is read from the 2D canvas at the end of the frame. In
    VR the world is in the eye images, not the canvas, and under a menu it isn't drawn at all, so the
    thumbnail was the HUD on black. On that frame (`RE_RequestSaveThumb` pending, the frame held) the world
    is now drawn flat into the canvas under the HUD, as on the flat screen; the headset doesn't see it,
    because the 2D layer re-shows its last image. The hold lasts at least until that frame (up to
    `VR_THUMB_WAIT`, 4 frames), even with `vr_menuSettle` 0 or 1 (`R_VR_SceneEyes` in `tr_vr.c`).
  - **The laser:** while a menu is up (`vr_menuLaser`), a thin red quad layer is submitted after the 2D quad,
    from the weapon hand to where its ray meets the menu's plane. It is a second small swapchain (16x16,
    filled once with red), a strip 6 mm wide turned about its own line to face the head, placed in the
    reference space; the engine's input (`VR_MenuPointer`) sets its two ends each frame (`VR_SetMenuLaser`).
- **Dimmer.** A 16×16 swapchain cleared to black with alpha = the dim value, shown as a head-locked
  8 m × 8 m quad 1 m away between the projection and the 2D quad. It is cleared only when the value changes.
  (`XR_KHR_composition_layer_color_scale_bias` would do this without a layer, but the simulator lacks it.)
- **Detection.** `cl_scrn.c` sets `vr_uiState` to 1 while a UI owns the keys (the UI VM or the SP UI)
  or a loading or connecting screen is up, 2 while a Bink or in-game cinematic plays, else 0. The renderer reads `cg_cameraActive` for scripted
  cameras. No game-module change.
- **No pillarbox bars on the HUD.** In play, `SCR_DrawScreenField` blacks out the sides before the
  world is drawn over them (`cl_viewmode 1`). In VR the world isn't in the 2D image, so those bars
  would be opaque slabs beside the HUD; the client skips them in play while the renderer reports
  `vr_active 1` (a read-only cvar the renderer sets while frames go to the headset).

## Cutscenes: virtual screen

- **Scripted cameras** (`cg_cameraActive`) **and cinematics** (`vr_uiState` 2): the scene is drawn flat,
  with the game's own camera and FOV, into the 2D image, and the 2D image is shown opaque on a big screen,
  `vr_theaterWidth` 4.4 m wide at `vr_theaterDistance` 4 m, world-locked where the head faced when the
  cutscene began. The world behind is frozen and dimmed to `vr_theaterDim` 0.85. The camera never drives
  the head. `vr_cutsceneScreen 0` turns this off (cameras then render in stereo).
- **Forced view angles** outside a camera rotate the body yaw only. Screen shake in VR is still to do.

## Cvars

| Cvar | Default | Notes |
|---|---|---|
| `vr_enable` | 1 Android, 0 desktop | latched |
| `vr_worldScale` | 32 | game units per metre |
| `vr_resolutionScale` | 1.0 | latched; 0.5 to 1.5 times the recommended eye size, rounded to a multiple of 8 |
| `vr_msaa` | 4 | latched; eye MSAA samples |
| `vr_refreshRate` | 72 | latched; Hz, the closest offered rate |
| `vr_hudDistance`, `vr_hudWidth`, `vr_hudHeight`, `vr_hudFollow`, `vr_hudFollowAngle` | 1.2, 1.2, −0.1, 2, 30 | HUD quad |
| `vr_menuDistance`, `vr_menuWidth`, `vr_menuFollow`, `vr_menuFollowLag`, `vr_menuHeadLocked`, `vr_menuDim`, `vr_menuLaser` | 0.7, 0.9, 1, 0.25, 0, 0.6, 1 | menu quad, and its laser |
| `vr_debugFrames` | 0 | log what every frame submits; `vr_screenshot <n>` writes n frames in a row, numbered by that frame |
| `vr_cutsceneScreen`, `vr_theaterDistance`, `vr_theaterWidth`, `vr_theaterDim` | 1, 4.0, 4.4, 0.85 | big screen |
| `vr_uiState` | | read-only, set by the client |
| `vr_active` | | read-only, set by the renderer: 1 while frames go to the headset |
| `vr_debugCapture` | 0 | latched; lets `vr_screenshot` copy the eye images |
| `vr_debugHeadTurn` | | testing: a pretend head turn, "yaw pitch roll" in degrees |

Commands: `vr_recenter`; `vr_screenshot` writes `screenshots/vr_left.tga`, `vr_right.tga` and
`vr_canvas.tga` (needs `vr_debugCapture 1`, which adds transfer-source usage to the eye swapchains).

The module logs once per run the eye FOVs and separation, each session state, and every 10 s the frame
rate, the share of fresh and frozen world frames, and the average layer count.

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
2. **Render straight to the eye images.** Done: no eye blits, the 2D pass is one small copy.
3. **Skip the full-screen post passes in VR.** Done: bloom, HDR, screen map and the gamma pass are off
   for the eyes. Menus and cutscenes don't render the world in stereo at all (frozen projection).
4. **Fixed foveation (planned):** `XR_FB_foveation` plus `XR_FB_foveation_vulkan` (the runtime hands a
   fragment-density-map image per swapchain image), with the level from `XR_FB_foveation_configuration`.
   The eye render pass then needs `VK_EXT_fragment_density_map` and the map as an extra attachment, so it
   wasn't cheap enough for this phase. The simulator offers `XR_FB_foveation`.
5. **Refresh rate:** done. `vr_refreshRate` 72 through `XR_FB_display_refresh_rate`; 80/90 Hz once there
   is headroom.
6. **Resolution scale:** done. `vr_resolutionScale` (0.5 to 1.5) on the recommended eye size. `vr_msaa`
   2 is the next lever after it.
7. **Multiview later:** only if profiling shows the CPU recording both eyes is the limit.
   - Option A: `VK_KHR_multiview` with one array swapchain, a two-layer render pass and
     per-view matrices in the shaders.
   - Option B: record the shared front end once and replay the back end per eye.

   Both touch every pipeline, so they wait for data.

## Testing on the Mac with the Meta XR Simulator

1. Build the Khronos loader once: `macos/build-openxr-loader.sh <OpenXR-SDK-Source-1.1.65> <build dir>`.
2. Build an `EF_VR=ON` engine and app (with its own `APP_NAME` and `BUNDLE_ID`).
3. Launch with `EF_OPENXR_LOADER=<build dir>/src/loader/libopenxr_loader.dylib` and
   `XR_RUNTIME_JSON=<MetaXRSimulator.app>/Contents/Resources/MetaXRSimulator/meta_openxr_simulator.json`,
   plus `+set vr_enable 1`. No system-wide runtime switch is needed. Use `+map` (not `+devmap`, which
   starts Holomatch) or `+load`.
4. The simulator's window shows one eye with the layers composited; `vr_screenshot` gives both eyes.

Results so far (Quest 3 profile, 1680×1760 per eye, 4× MSAA, Apple M-series via MoltenVK): the session
reaches FOCUSED, holds 70 to 72 fps at the 72 Hz display rate in play, and the HUD, menu and
big-screen layers behave as described. With `EF_VR` built in and no runtime, or `vr_enable 0`, the game
stays flat.

## Effects in VR

The game's effects were made for one flat view. In VR the eyes look where the head looks, which
can be far from the body's view, so anything turned to (or culled by) the flat view would show
edge-on or vanish.

- **Renderer: already per eye.** Each eye is its own view (`R_VR_SetupEye`), and the back end builds
  `RT_SPRITE`, `RT_ORIENTEDSPRITE`, `RT_LINE`, `RT_ORIENTEDLINE`, `RT_LINE2`, `RT_BEZIER`,
  `RT_CYLINDER`, `RT_ELECTRICITY`, beams and rails, and `deformVertexes autosprite`/`autosprite2`,
  from the view being drawn (`backEnd.viewParms`). So these face each eye exactly, and entities
  and the world are culled against each eye's own frustum. Holomatch's effects are mostly these
  entity types, so they look right with no game-module change.
- **Flares** (`vk_flares.c`): each eye keeps its own flare records. Before, both eyes shared one
  record per surface, so one eye's occlusion test decided the other's fade, and an eye could throw
  out the other's flares as stale. Flat builds are unchanged (no eye, the same test).
- **Single player's effects system** (`fx_primitive.cpp` and the `FX_*`/`fx_*` files): its sprites,
  particles, lines, electricity, trails, cylinders and Bézier beams are polygons built in the cgame
  from the view, once a frame, and it skips effects behind the view or nearer than 16 units. In VR
  they now use the **head** (`CG_FXViewOrg`, `CG_FXViewAxis` in `cg_vr.cpp`): between the eyes, turned
  as the headset is. The head rather than each eye, because the polygons are built once for both
  eyes; the eyes' 6 cm only changes the angle by a few degrees. Cutscene cameras keep the flat view
  (they are drawn flat on the big screen). The near cut is 6 units in VR, as the gun is in the hand.
  Explosion models that are turned to the viewer when they spawn use the head's position too.
  Without VR the accessors return `cg.refdef`'s own vectors, so flat play is identical.
- The engine now hands the cgame the head's orientation with its position (`VR_BODY_HEAD`).

Testing: `vr_debugHeadTurn "yaw pitch roll"` (degrees) turns the located eye views about the point
between the eyes, as if the head had turned, so effects can be checked looking away from the body
without a headset.

## Known issues

- The 2D alpha is a coverage approximation (see The 2D layer).
- Entering an eye pass twice in one frame (a second world scene, such as a portal camera view drawn
  as its own scene) clears that eye again; the last scene wins. Not seen in SP so far.
- Pipeline prewarming doesn't cover `RENDER_PASS_VR_EYE` yet, so the first frames compile pipelines.
- `r_smp 1` still disables VR.

## Open questions for the headset

- Does the Quest runtime accept the renderer's device features and extensions as they are? The
  device-create call goes through the runtime, which adds its own.
- Swapchain format and sRGB behaviour on the real compositor (see Colour space).
- GPU time per eye on the Quest 2 at the recommended size with 4× MSAA.
- Which loader the APK carries: the Khronos loader `libopenxr_loader.so` from the Khronos release AAR.
  Meta documents that the Khronos loader 1.0.34 or newer works on Quest OS v62 or newer.
