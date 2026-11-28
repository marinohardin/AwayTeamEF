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
- **VR co-op (vr-coop, 2026-11-26)**, Meta XR Simulator plus a flat Mac copy, both roles (VR host with a flat guest, flat host with a VR guest): the head's sideways offset drives the game's lean as an analog value in spare command bits, through the key lean's wall trace, so the flat partner sees the lean stance; a VR guest's hand muzzle and ray go with its commands and the host fires its shots from there (the hit matches the guest's laser); flat partners see a VR player's head and torso turn toward the hand's aim. Flat-only co-op sends and draws the same as before. Design: [COOP.md](COOP.md). Not yet on a headset, and not across a real network.
- **Clean shutdown (vr-quit, 2026-11-26):** quitting with the Meta XR Simulator attached crashed, because the session was never properly ended and the Vulkan device was destroyed before the XR instance: the simulator then freed its queue wrapper on a destroyed device. Now the session is ended through STOPPING and the whole OpenXR side is destroyed before the device and instance. Quit, `vid_restart` and map changes run without a crash in the simulator. When the runtime ends the session itself, the Quest build quits and a desktop build carries on flat. Android pause and resume are reviewed only (no headset).

- **Effects (vr-fx, 2026-11-26)**, in the Meta XR Simulator on the Mac: the renderer already turns sprites, beams, lines, electricity and autosprite shaders to each eye and culls per eye (so Holomatch's entity effects need no change); flares now keep separate records per eye. Single player's effects system (cgame polygons) faces and culls against the real head instead of the body's flat view, so effects no longer go edge-on or vanish when the head looks away. `vr_debugHeadTurn` fakes a head turn for testing. Design: [RENDERING.md](RENDERING.md#effects-in-vr). Not yet on a headset.

- **Holomatch H1, round 1 (vr-hm, 2026-11-26)**, in the Meta XR Simulator on the Mac against a local server, with cMod's and the stock 1.2 cgame: aiming from the hand on any server without changing the protocol, the usercmd, the game modules or the userinfo. The engine keeps the body yaw apart from the aim, converges the hand's ray onto the view angles it sends (its own trace through the snapshot's world and solid entities, a muzzle table for the server's shot origin, an eye-to-muzzle check), clamps the pitch and sends no roll, and draws a laser plus a dot where the shot will really land (orange when the barrel is blocked). It turns off the view bob and the crosshair while active and never saves those values. Shots land within 1 unit of the dot on walls up to 760 units, blocked shots too (one outlier, where the aim moved before the shot). Spikes S1, S2 and S4 and the results: [HOLOMATCH-H1.md](HOLOMATCH-H1.md#10-round-1-what-the-spikes-and-the-aim-tests-found-2026-11-26). Not yet: a bot as the target, the view weapon at the hand, the head lean, observers (round 2), and a headset.
- **Holomatch H1, round 2 (vr-hm2, 2026-11-27)**, same setup, both cgames: the view weapon at the tracked hand with the wrist roll, its barrel along the laser, beams, muzzle flashes, puffs and lights following it, sounds heard from the head (engine-side scene interception, no game module change). The head leans within 4 units, then walks the body through the usercmd (walk button, at most 64), stopped at ledges and walls with a fade; real crouch. The Holomatch controller map, snap turns spread at 15° per command, the live world behind in-match menus, and the theater screen for following another player. Flat observers see the model face the aim with no roll, step out, crouch and turn. 15 of 15 shots hit a still flat player at 200 units with five weapons; a 20-minute match ran without drops. Flat gates ALL IDENTICAL. Details: [HOLOMATCH-H1.md](HOLOMATCH-H1.md#11-round-2-the-gun-at-the-hand-the-head-the-controls-and-the-ui-2026-11-27). Not yet: the lag compensation matrix, player hits beyond 200 units, the intermission theater capture, and a headset.

- **Weapon wheel (vr-wheel, 2026-11-27)**, in the Meta XR Simulator on the Mac, in single player, co-op (VR host with a flat guest, flat host with a VR guest) and on a local Holomatch server: holding the right thumbstick click opens a ring of the player's weapons with the game's HUD icons, world-locked in front of the head; the weapon hand points (or the right stick picks), the name shows in the middle, a haptic tick per segment, release picks with the ordinary `weapon <n>` command, the middle cancels, a tap keeps its old meaning, empty weapons are marked. Single player only, time slows to 0.2 while it's open (measured: about 1000 ms of game time in 4.5 s) and is back to exactly normal on a pick, a cancel, the menu, a save, a load, a map change, death, a cutscene camera and quit; co-op (both roles) and Holomatch never change time. Controls: [QUEST.md](QUEST.md#controller-map). Not yet on a headset: real triggers held across the wheel, the haptic tick, comfort of the slowed world and room-scale steps while slowed.
- **Headset fixes, controls and shell (vr-fix-a, 2026-11-27)**, from the owner's first session on a Quest 2: the owner's controller design (weapon hand trigger fires and its grip is alternate fire, the other hand's trigger jumps and its grip crouches, X use, A Enter and skip a cutscene, B or Y the objectives, right stick up/down changes weapon, the left menu button is Escape; Holomatch keeps its item, scores and objectives on A, B and Y), snap turns eased over 0.1 s, the menu cursor following the weapon hand's ray, no touch overlay in the Quest build, a four-button start screen (Single player, Co-op host, Co-op join, Holomatch) with co-op now compiled into the Quest APK, and a Known issues list. Checked in the Meta XR Simulator and by building the APK; not yet on a headset. Details: [QUEST.md](QUEST.md).

## Testing from a Mac

- **Meta Spatial Simulator (arm64):** an Android virtual device in Android Studio. Checks that the APK installs and starts, as a flat app.
- **Meta XR Simulator (Mac):** an OpenXR runtime on the desktop, possibly for exercising the VR code path. Unverified with MoltenVK (spike 3).
- **The real Quest 2:** developer mode, USB-C and `adb install`.

## Owner notes

- The working machine is a Mac with little free disk space. Large downloads (simulators, system images, SDK pieces) need the owner's OK first.
