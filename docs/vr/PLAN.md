# Quest VR: goal and plan

Branch: `feature/quest-vr`. Status: started 2026-11-26.

## Goal

A native **Meta Quest 2** build of AwayTeamEF with full **6DoF**: head position and rotation, and tracked Touch controllers. The headline feature is **VR co-op**: a Quest player plays the single-player campaign with a friend on a flat Mac, Windows or Linux PC.

## Ground rules

1. **Flat-screen players are never affected.**
   - VR code is compiled only into the Quest build (CMake option `EF_VR`, off by default), or runs only when VR is active.
   - Desktop builds must behave and perform exactly as before. The regress gates (`base-1_4_0`, `coop-m2-base`) must stay ALL IDENTICAL on every merge.
2. **Single player:** anything goes inside the VR build.
3. **Co-op:** a VR host or guest must play with non-VR clients on Mac, Windows and Linux.
   - We ship both ends, so flat clients can learn to show VR-only things, such as a partner's real lean, or hand and weapon poses.
   - Older AwayTeamEF clients must still connect. New data is optional and announced; it is never required.
4. **Holomatch:** must stay compatible with servers and clients that aren't ours.
   - **H1 (any server).** The usercmd and protocol are unchanged. Controller aim goes out as normal view angles. The head offset is applied only locally and clamped. Leaning past a small dead zone walks the whole body, so others see a normal player step out; where the body can't move, the view fades. A laser shows where the server's shot will really land. The game already steers shots to the crosshair, which helps keep the hand and the shot together.
   - **H2 (our servers only, later).** VR extras such as a hitbox that really leans, hand muzzles and pose drawing, negotiated with clients that announce support. This would live in a fork of the Holomatch game modules; check that code's license and get the cMod author's OK before shipping one.
   - **Never:** change the usercmd or protocol for everyone, allow unclamped leaning, or bring single player's slow-motion into Holomatch.
5. **Code hygiene:**
   - Our own code and our own UI.
   - Stay on Vulkan.
   - No game assets in the repo.
   - Small vendored headers (such as the Khronos OpenXR headers, Apache-2.0) are fine, with their licenses listed in `THIRD_PARTY_LICENSES.md`.

## Architecture (initial)

- **Platform:** Android arm64 APK, OpenXR through the Quest's own OpenXR loader, and Vulkan through `XR_KHR_vulkan_enable2`. OpenXR creates the Vulkan instance and device, and the renderer accepts them instead of making its own.
- **Rendering:** one pass per eye into OpenXR swapchain images, sharing culling where possible.
  - The 2D HUD and menus go on a quad layer or on a virtual screen in front of the player.
  - Cutscenes and scripted cameras use a comfort-safe virtual screen.
- **Input:** Touch controllers mapped to the existing button and axis cvars, plus head and hand poses.
  - Single player and co-op get a hand-aimed muzzle, with an eye-to-hand trace so you can't shoot through walls.
  - Comfort options: snap or smooth turning, and a vignette.
- **Co-op:** the VR player's sideways head offset drives the existing lean (`leanofs`), so flat partners see it through the normal replication. Hand poses are optional extra data that flat clients can draw.
- **Android shell:** the tree started as an Android build, and the Android-only parts were removed when the project went cross-platform. The Quest build restores a minimal Android shell (NDK and CMake, a native activity, the APK manifest for Quest) on top of the current shared code. Gradle is not required.

## Phases

| # | Phase | Done when |
|---|---|---|
| 0 | Spikes | Each risk below has a yes or no and a written finding |
| 1 | Restore the Android build | A flat arm64 APK builds from the current tree on this Mac and starts in an Android or Quest simulator; later on the Quest 2 |
| 2 | VR single player on Quest | Stereo 6DoF, hand weapons, menus and comfort; a whole level is playable at 72 Hz |
| 3 | VR co-op | A Quest player and a flat Mac player finish a level together; flat clients see lean and aim |
| 4 | Holomatch H1 | A Quest player plays on a stock server, and stock clients see a normal player |
| 5 | Holomatch H2 (optional) | Only after phase 4, and with the cMod author's OK |

**Spikes:**
1. Can the Vulkan renderer accept an instance and device that OpenXR created? Sketch the stereo render path.
2. Rebuild the Android shell from the current tree: CMake, NDK and an APK, without Gradle if possible.
3. Does the Meta XR Simulator (Mac) work with a Vulkan app through MoltenVK? If yes, VR code can be tested on the Mac.
4. Does the co-op `leanofs` replicate to a flat partner? Tested with two flat copies on the Mac.
5. Can the engine override the view and weapon in Holomatch without changing the game modules? Tested on a flat Mac build.

## Status

- **Spikes 1, 2 and 4 done** (2026-11-26): the renderer accepts an OpenXR-created Vulkan instance and device (compiles; not run against a runtime, [RENDERING.md](RENDERING.md)); a flat arm64 APK builds without Gradle and reaches the main menu in the emulator; the co-op lean replicates to a flat partner. Spike 3 (Meta XR Simulator, about 450 MB) waits for the owner's OK. Spike 5 is open.
- **Phase 1 done** in the emulator: the flat APK (`android/make-apk.sh`). Not yet on the Quest.
- **Phase 2 started** (2026-11-26): the Quest APK (`VR=1 android/make-apk.sh`, [QUEST.md](QUEST.md)):
  - package `org.awayteamef.quest` with the Quest manifest, `EF_VR=ON`, and the Khronos OpenXR loader 1.1.65 built from source with the NDK;
  - controller input through OpenXR actions (Touch controller, simple controller fallback): move, snap or smooth turn, fire, alternate fire, jump, crouch, use, menu, weapon cycling, menu cursor; head, aim and grip poses exposed for hand aiming;
  - in the emulator (no OpenXR runtime) it stays flat with "no OpenXR runtime", no crash. **Not yet run on a headset.**
  - Next: first run on the Quest 2, then render straight into the eye images, the 2D layer for HUD and menus, and aiming from the hand.
- **Movement (vr-move, 2026-11-26)**, in the Meta XR Simulator on the Mac, single player: 6DoF head (lean as a view offset within `vr_leanRadius`), room-scale steps through the game's collision with the eye pushed back and faded in walls, real crouch (`vr_crouchThreshold`) and button crouch, analog stick speed with head- or off-hand-relative direction, snap or smooth turn pivoting about the head, optional comfort vignette, recentre on a held left thumbstick click. The key lean is off in VR. Design and cvars: [MOVEMENT.md](MOVEMENT.md). Not yet on a headset; Holomatch and co-op guests get the lean without a collision move.
- **Hands (vr-hands, 2026-11-26)**, in the Meta XR Simulator on the Mac, single player: the controller pose (position and full orientation, roll included) reaches the cgame and the game through a VR-only cgame syscall, relative to the head and turned by the body yaw. The view weapon sits in the dominant hand, oriented by it, with no bob or sway (`vr_leftHanded` swaps hands; per-weapon grip table plus `cg_vrGunAdjust` for tuning). Shots leave the muzzle in the hand's direction, with an eye-to-muzzle trace so a gun pushed into a wall can't shoot through it (hitscan and projectiles). A laser and dot at the real hit point (`cg_vrLaser`), no screen-centre crosshair in VR, and a marker for the off hand (`cg_vrOffHand`). Not yet on a headset: the controller grip offsets, the hypos' fit, left-handed play, co-op guests (the hand is drawn but the host fires from the view) and Holomatch are unverified.
- **Clean shutdown (vr-quit, 2026-11-26):** quitting with the Meta XR Simulator attached crashed, because the session was never properly ended and the Vulkan device was destroyed before the XR instance: the simulator then freed its queue wrapper on a destroyed device. Now the session is ended through STOPPING and the whole OpenXR side is destroyed before the device and instance. Quit, `vid_restart` and map changes run without a crash in the simulator. When the runtime ends the session itself, the Quest build quits and a desktop build carries on flat. Android pause and resume are reviewed only (no headset).

## Testing from a Mac

- **Meta Spatial Simulator (arm64):** an Android virtual device in Android Studio. Checks that the APK installs and starts, as a flat app.
- **Meta XR Simulator (Mac):** an OpenXR runtime on the desktop, possibly for exercising the VR code path. Unverified with MoltenVK (spike 3).
- **The real Quest 2:** developer mode, USB-C and `adb install`.

## Owner notes

- The working machine is a Mac with little free disk space. Large downloads (simulators, system images, SDK pieces) need the owner's OK first.
