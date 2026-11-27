# Quest VR build: build, install, controls

Status: 2026-11-26. The Quest APK builds and installs. Without an OpenXR runtime (the Android
emulator) it runs flat and logs `VR: no OpenXR runtime (-51), staying flat`. **It has not run on
a headset yet**, so everything below about the Quest itself is untested.
See [PLAN.md](PLAN.md) for the plan and [RENDERING.md](RENDERING.md) for the stereo path.

## Build (macOS, no Gradle)

Needs the Android SDK (`~/Library/Android/sdk`: an NDK, build-tools, a platform), cmake, ninja,
a JDK and Python 3. The SDL tarballs come from `build-deps/`, as for the flat APK.

```sh
VR=1 android/make-apk.sh        # -> android/build/AwayTeamEF-Quest-<version>-arm64.apk
android/make-apk.sh             # the flat phone APK, unchanged -> build-android/
```

What `VR=1` changes, compared with the flat APK:

| | Flat | Quest (`VR=1`) |
|---|---|---|
| Package | `org.awayteamef.app` | `org.awayteamef.quest` (both can be installed) |
| Manifest | `android/AndroidManifest.xml` | `android/AndroidManifest-quest.xml` |
| Activity | `EFActivity` | `QuestActivity` (`android/java-quest`): passes the JavaVM and activity to the vr module, keeps the screen on, hides the system bars |
| Engine | `EF_VR=OFF` | `EF_VR=ON`: `efcode/vr` (OpenXR session, controllers) and the renderer's VR hooks, plus `android/jni/quest_jni.c` |
| OpenXR loader | none | `libopenxr_loader.so`, built by the script with the NDK from the Khronos **OpenXR-SDK-Source release-1.1.65** source tarball (about 5 MB, Apache-2.0, checksum-pinned in `make-apk.sh`, downloaded once into `android/build/src/`). It matches the headers in `deps/openxr/include` |

The Quest manifest adds: the launcher category `com.oculus.intent.category.VR`, the OpenXR
permissions (`org.khronos.openxr.permission.OPENXR`, `OPENXR_SYSTEM`) and `<queries>` for the
runtime broker and services, `com.oculus.supportedDevices` = `quest2|questpro|quest3|quest3s`,
`android.hardware.vr.headtracking` (required), `com.oculus.vr.focusaware`, landscape, a
fullscreen theme and no resizing.

Size (1.5.0): flat 3.1 MB; Quest 3.2 MB (the loader adds about 0.4 MB stripped).

## Install on a Quest 2

1. **Developer mode.** Register as a developer (a free Meta developer organization), then in the
   Meta Horizon phone app: Devices → the headset → Headset settings → Developer mode → on.
   Reboot the headset.
2. Connect it with a USB-C data cable, put it on and accept **Allow USB debugging** (tick
   "Always allow from this computer"). `adb devices` should list it.
3. Install, and push the game data (the four paks from an installed Elite Force; the app's own
   OBB folder needs no storage permission):

   ```sh
   adb install -r android/build/AwayTeamEF-Quest-1.5.0-arm64.apk
   adb shell mkdir -p /sdcard/Android/obb/org.awayteamef.quest/baseEF
   adb push pak0.pk3 pak1.pk3 pak2.pk3 pak3.pk3 /sdcard/Android/obb/org.awayteamef.quest/baseEF/
   ```

   Configs and saves go into the same folder. **Uninstalling the app deletes it**, saves included.
4. Start it from the headset's library (Unknown Sources), or:

   ```sh
   adb shell am start -n org.awayteamef.quest/org.awayteamef.app.QuestActivity
   adb shell am start -n org.awayteamef.quest/org.awayteamef.app.QuestActivity -e args "'+map borg1'"
   adb logcat -s VoyagerSP AwayTeamEF OpenXR-Loader
   ```

   Look for `VR: OpenXR runtime ...`, `VR: session running` and
   `VR input: right hand uses /interaction_profiles/oculus/touch_controller` in the log.

## Controller map

OpenXR action set `gameplay`, with suggested bindings for `/interaction_profiles/oculus/touch_controller`
and the fallback `/interaction_profiles/khr/simple_controller`. The engine turns the actions into
the commands, key events and joystick axes it already has, so the game modules see ordinary input.

**In play:**

| Control | Action | Engine input |
|---|---|---|
| Left thumbstick | Move and strafe, speed in proportion to deflection ([MOVEMENT.md](MOVEMENT.md)) | usercmd forward / right |
| Right thumbstick, left/right | Turn: snap by default, smooth as an option | view yaw |
| Right trigger | Fire | `+attack` |
| Left trigger | Alternate fire | `+altattack` |
| A | Jump | `+moveup` |
| B | Crouch (standing players can also really crouch) | `+movedown` |
| X | Use (in VR the head is the lean; Use + strafe no longer leans) | `+use` |
| Y, or the left menu button | Menu | Escape |
| Right thumbstick click | Next weapon | `weapnext` |
| Left thumbstick click | Previous weapon; hold 0.7 s to recentre | `weapprev` / recentre |
| Grips | Not mapped yet (kept free for grabbing and two-handed weapons) | |

Alternate fire is on the **left trigger**, not the right grip, so that each trigger is one fire
mode and the grips stay free for hand interactions later.

**In menus and the console:** the right thumbstick moves the mouse cursor, the right trigger
clicks, the left thumbstick sends arrow keys, A is Enter, and B, Y and the menu button are Escape.
A button held while a menu opens or closes does nothing in the new mode until it is released.

**Simple controller (fallback):** right select = fire, left select = use, either menu button = menu.
There is no movement on that profile.

**Poses:** the head (OpenXR VIEW space), and per hand the aim pose (the pointing ray) and the grip
pose, are located every frame in the same space as the eye views. `VR_GetInputState()` and
`VR_GetHandPose()` (`efcode/vr/vr_input.h`) expose them for hand aiming; nothing uses them yet.

**cvars (archived):**

| cvar | Default | Meaning |
|---|---|---|
| `vr_turnMode` | 0 | 0 = snap turn, 1 = smooth turn |
| `vr_snapTurnAngle` | 45 | degrees per snap |
| `vr_turnSpeed` | 150 | smooth turn, degrees per second at full deflection |
| `vr_stickDeadzone` | 0.25 | thumbstick dead zone (0..0.9) |
| `vr_menuCursorSpeed` | 500 | menu cursor speed, pixels per second |
| `vr_leftHanded` | 0 | 1 = the weapon in the left hand (the triggers swap with it) |
| `cg_vrLaser` | 1 | 0 = off, 1 = laser and dot at the hit point, 2 = dot only |
| `cg_vrOffHand` | 1 | draw a marker at the off hand |
| `vr_roomScale`, `vr_leanRadius`, `vr_seated`, `vr_crouchThreshold`, `vr_moveDirection`, `vr_comfortVignette` | | head position, room-scale, crouch, move direction and comfort: see [MOVEMENT.md](MOVEMENT.md) |

Movement follows the stick's deflection and, by default, the head's yaw; turning pivots about the
head. Details in [MOVEMENT.md](MOVEMENT.md).

## Code

| Piece | Path |
|---|---|
| Actions, bindings, sync, poses (OpenXR only) | `EFAndroid-SP/app/jni/efcode/vr/vr_input.c`, `vr_input.h`, `vr_local.h` |
| Controller map onto the engine (compiled into the engine target) | `efcode/vr/vr_input_engine.c` |
| Session wiring | `efcode/vr/vr_openxr.c`: `VR_Input_*` calls in `VR_Init`, `VR_Shutdown`, `VR_CreateSession`, `VR_DestroySession`, `VR_PollEvents` and `VR_BeginFrame` |
| Android shell | `android/AndroidManifest-quest.xml`, `android/java-quest/`, `android/jni/quest_jni.c`, `android/CMakeLists.txt` (`EF_VR`), `android/make-apk.sh` (`VR=1`) |
