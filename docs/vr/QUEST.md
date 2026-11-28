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
| Activity | `EFActivity` | `StartActivity` (the one launcher entry: a flat panel with four buttons, see [the start screen](#the-start-screen)) and `QuestActivity` (`android/java-quest`, in its own `:game` process): passes the JavaVM and activity to the vr module, copies cMod's paks for the engine, goes back to the start screen at Quit, keeps the screen on, hides the system bars |
| Engine | `EF_VR=OFF`, no co-op | `EF_VR=ON`: `efcode/vr` (OpenXR session, controllers) and the renderer's VR hooks, plus `android/jni/quest_jni.c`; and co-op (`EF_COOP`: `sys_coop_host.c`, `sys_coop_net.c`, the game's `coop/`), which the start screen's Co-op buttons need |
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
3. Install it, then add the game files. Either in the headset: copy a CD image (`.iso`, or `.bin`
   with its `.cue`), a `BaseEF` folder or GOG's installer into the Quest's Download folder (USB cable,
   or the Quest's browser for GOG), and use **Choose…** on the start screen
   ([the start screen](#the-start-screen)); or push the paks from a computer (the app's own OBB
   folder needs no storage permission):

   ```sh
   adb install -r android/build/AwayTeamEF-Quest-<version>-arm64.apk
   adb shell mkdir -p /sdcard/Android/obb/org.awayteamef.quest/baseEF
   adb push pak0.pk3 pak1.pk3 pak2.pk3 pak3.pk3 /sdcard/Android/obb/org.awayteamef.quest/baseEF/
   ```

   Configs and saves go into the same folder. **Uninstalling the app deletes it**: the copied game
   files go (as they should), and so would the saves, unless the start screen backs them up (its
   Saves & Settings group, below).
4. Start it from the headset's library (Unknown Sources). The app has one entry, which opens the
   start screen; or from the computer:

   ```sh
   adb shell am start -n org.awayteamef.quest/org.awayteamef.app.StartActivity
   adb shell am start -n org.awayteamef.quest/org.awayteamef.app.QuestActivity                 # straight into the game
   adb shell am start -n org.awayteamef.quest/org.awayteamef.app.QuestActivity -e args "'+map borg1'"
   adb logcat -s VoyagerSP AwayTeamEF OpenXR-Loader
   ```

   Look for `VR: OpenXR runtime ...`, `VR: session running` and
   `VR input: right hand uses /interaction_profiles/oculus/touch_controller` in the log.
   Starting QuestActivity directly skips the start screen and the engine's mode arguments (it
   starts single player, as before).

### The start screen

Before the game, a flat panel (a plain Android activity, `StartActivity`, in the app's main
process) made for the controller pointer: big buttons, and its own keyboards on the panel (the
Quest's system keyboard is never opened; it hung the app when an old text field summoned it). Its
look and wording follow the desktop launchers: the header with the title lettering
(`tools/launcher-titles/png`, pictures of the lettering; no font file is shipped), the modes on the
left, Player, Game Files and Saves & Settings on the right, and "AwayTeamEF for Meta Quest
<version>" at the bottom. The system Back button goes back a page.

| Button | Starts the game with | Then |
|---|---|---|
| Single Player: Play | `+set com_holomatch 0` | the game's main menu |
| Co-op: Host | `+set com_holomatch 0 +set sp_coop 1 +set coop_name "<name>"` | start or load a mission; the guest needs your headset's IP address (the Wi-Fi settings show it) |
| Co-op: Join… | (a page) | see below |
| Holomatch: Browse Servers | (a page) | see below |
| Holomatch: Play Holomatch | `+set com_holomatch 1 +set name "<name>"` | the game's own Holomatch menus (bots, its own browser) |

All of them need pak0, pak1 and pak2 (patch 1.2); until then they are greyed and the Game Files
group says what is missing. These are the arguments the desktop launchers put on the command line
(`efcode/launcher/lc_launch.c`).

- **Player name**: one name for Holomatch (`name`) and co-op (`coop_name`). Tap it for the on-panel
  keyboard: letters, digits, symbols, and a Colours layer with the game's colour codes `^0`–`^7`
  (a preview shows the result). Cleaned like the desktop launchers' (ASCII without `" + ; \ %`, at most
  31 characters). The first time it is `hmconfig.cfg`'s name, else "Crewman" (the desktop default
  without a user name).
- **Game Files**: where the files are (`Android/obb/org.awayteamef.quest/baseEF`), Ready / Patch 1.2
  missing / No game files yet, a chip per pak (pak0–pak3), and whether the **Expansion Pack** is
  there (Virtual Voyager's first map in a pak, the desktop's test). **Choose…** / **Add Game Files…**
  opens the Game Files page: **CD image** (`.iso`, or `.bin` and its `.cue` picked together; the
  game's CD brings pak0, the Expansion Pack's CD brings pak1–pak3), **BaseEF folder** (its `.pk3`
  files), or **GOG installer** (`setup_….exe`, unpacked, nothing installed). Each opens Android's
  file picker (so no storage permission). The copy is the desktop launchers' own code
  (`efcode/launcher` and `sys_iso.c`, `sys_osx_gog.c` in `liblauncher.so`, through
  `LauncherCore.java` / `android/jni/launcher_jni.c`): the same checks, progress panel with Cancel,
  rollback on failure, "Nothing to add" and "Not enough disk space" alerts. Picked files reach the C
  code by name: `GameImport.java` opens each one and registers its descriptor under a path named like
  the file, and `liblauncher.so` serves the core's `fopen` / `open` / `stat` of that path from the
  descriptor (linked with `--wrap`; reopening `/proc/self/fd/<n>` of shared storage is refused).
  Android's folder picker won't hand out Download itself, so a BaseEF folder goes in a folder of its
  own there (Download/EF/BaseEF, say). make-apk.sh builds liblzma (xz 5.8.4, checksum-checked) for GOG's installer.
- **Saves & Settings**: uninstalling deletes the OBB folder, saves included. The start screen copies
  everything in it except `.pk3` files (and logs) into a folder you pick once (Android's folder picker;
  suggested `Documents/AwayTeamEF`), every time it comes back from the game and with Back Up Now;
  only changed files are copied, nothing in the backup is deleted, and no backup runs while a
  restore does (`SaveBackup.java`). **Other Folder…** moves future backups elsewhere. After a reinstall, **Restore…** and pick the same
  folder: Android lets a new install read the old install's files only through the picker. The panel
  explains this in a line of its own.

**Co-op: Join…** — left, **Search Local Network** (the desktop launchers' query, `getcoopinfo
<challenge> lan` as a UDP broadcast to port 29760, answered with `coopInfoResponse`; `CoopLan.java`,
off the UI thread, about 2.5 s) lists hosts (player and computer, mission or map, difficulty,
players, address); a tap joins, after asking for the password when the host set one. Hosts on
another version or full are greyed with the reason. A host whose "Show this game on my network" is
off (`coop_lanVisible 0`) does not answer; type its address. Under it, **Player 2**: Munro Male /
Female (`coop_sex`) and the character (`coop_look`, the crew and the Hazard Team from
`coop_characters.h`), as on the desktop Co-op sheet. Right, the host's address on a number pad
(digits, dot, colon; the last one is kept), **Password** (on-panel keyboard; passed as
`coop_password`, a CVAR_TEMP the game never saves, masked in the log), and **Join**:
`+set com_holomatch 0 +set coop_connect "<address>" +set coop_sex <male|female> [+set coop_look <id>]
+set coop_name "<name>" [+set coop_password "<pw>"]`. Co-op in the Quest build uses the same code
as on Linux, so both apps must be on the same co-op compatibility version (the 1.5.x line); a phone,
Mac, Windows or Linux partner works.

**Holomatch: Browse Servers** — the desktop launchers' server browser (servers.md §8, the same core):
tabs Local, Combined and Internet 1–5 (the default master servers), Refresh, the status line,
**Only favorites**, **Only servers with people**, and the columns star, Server (colour codes), lock,
Map (purple: an Expansion Pack map you don't have; amber: a map you don't have, which the server may
send), Players, Humans, Game, cMod, Ping; tap a header to sort. **Tap a server to join it**:
`+set com_holomatch 1 +set name "<name>" [+set password "<pw>"] +connect <ip:port>`; an Expansion
Pack map you don't have warns first, a locked server asks for its password. **Join by Address…**
takes an address on the number pad. Left out on the panel: the players table, Map Info and the 3D
preview.

### Like the desktop builds

The engine's desktop-only parts that have a Quest counterpart (`EF_QUEST`, defined for the Quest
engine by `android/CMakeLists.txt`; `EF_DESKTOP` itself stays off on Android):

- **Quit goes back to the start screen.** Quit in the single-player or Holomatch menus (a bare
  `quit`) ends the game and shows the start screen again, as the desktop builds go back to their
  launcher. The game runs in its own process (`android:process=":game"` on `QuestActivity`): after
  the engine's shutdown, `QuestActivity.quitToLauncher()` starts `StartActivity` (with the extra
  `org.awayteamef.quest.FROM_GAME` = true) and finishes, then the game process exits. Quitting from
  the headset's system menu, or the system closing the app, still ends the app.
- **Version.** The console, the `version` cvar and the server status answers say
  `AwayTeamEF for Meta Quest <version>`, like the desktop's `AwayTeamEF for Mac <version>`.
  `make-apk.sh` takes the version from `EFAndroid-SP/app/jni/engine/CMakeLists.txt`, as the desktop
  builds do, and also sets the APK's versionName from it. The `awayteamef` userinfo key stays
  desktop-only (Holomatch H1: a Quest player's userinfo stays as it is).
- **Loopback Host Fix.** A Holomatch server on the same network that a master server lists under the
  network's public address is listed as 127.0.0.1, as in the desktop launchers. With no launcher in
  the game's process, the game asks the router for the public address when it starts (the
  launchers' NAT-PMP/UPnP lookup, `efcode/launcher/lc_nat.c`); `+set cl_loopbackHostIP <ip>` overrides it.
- **cMod's game modules** by Noah Metzger (Chomenor), bundled with his permission
  ([macos/cmod/README.txt](../../macos/cmod/README.txt)): `make-apk.sh` puts the newest
  `macos/cmod/pakcmod-release-*.pk3` (or `CMOD_PAK=<file>`) into the APK's `assets/cmod/`, with the
  README and the game source license. `QuestActivity` copies the pk3 to the app's `files/cmod/` when
  the installed APK changes, and the engine loads it from there with the same checksum check against
  cMod's release list as on the desktop, so Holomatch uses cMod's cgame, ui and qagame by default
  (`+set cl_cmodModules 0` / `+set sv_cmodGame 0` turn them off).
- Still desktop-only: the `r_inGameVideo`, `m_filter` and `r_fbo` defaults and `Sys_PlatformPreInit`.

## Controller map

OpenXR action set `gameplay`, with suggested bindings for `/interaction_profiles/oculus/touch_controller`
and the fallback `/interaction_profiles/khr/simple_controller`. The engine turns the actions into
the commands, key events and joystick axes it already has, so the game modules see ordinary input.

**In play** (the weapon hand is the right one, or the left with `vr_leftHanded`, which swaps the
triggers and grips with the hands and nothing else):

| Control | Action | Engine input |
|---|---|---|
| Left thumbstick | Move and strafe, speed in proportion to deflection ([MOVEMENT.md](MOVEMENT.md)) | usercmd forward / right |
| Right thumbstick, left/right | Turn: snap (a short eased turn, 0.1 s) by default, smooth as an option | view yaw |
| Right thumbstick, up/down | Next / previous weapon, once per push | `weapnext` / `weapprev` |
| Right thumbstick click | Hold (0.25 s): the weapon wheel, see below. A tap does nothing (Holomatch: the last weapon) | `weapon <n>` |
| B | The weapon wheel, the same wheel: it opens at once and letting go picks (see below) | `weapon <n>` |
| Weapon hand trigger | Fire | `+attack` |
| Weapon hand grip | Alternate fire | `+altattack` |
| Other hand trigger | Jump | `+moveup` |
| Other hand grip | Crouch (standing players can also really crouch) | `+movedown` |
| A | Use (in VR the head is the lean; Use + strafe no longer leans) | `+use` |
| X | **Skip a cutscene** (the game's use-item key skips a camera scene; Enter skips a movie) | `+button2`, and the Enter key |
| Y | The mission objectives, shown while held | `+info` |
| Left thumbstick click | Hold 0.7 s to recentre. A tap does nothing | recentre |
| Left menu button | Menu | Escape |

In Holomatch (docs: [HOLOMATCH-H1.md](HOLOMATCH-H1.md) §6.1) the same shape holds, with these differences:
A is Use (`+use`, objects), **X is the holdable item** (`+button2`, no Enter: that game has an inventory item and
no cutscenes), **Y held shows the scoreboard and the objectives together** (`+info` and `+analysis`; B is the
wheel now, so the scoreboard moved to Y), and a tap of the left thumbstick click is the walk/run toggle.

**Weapon wheel** (every mode: single player, co-op and Holomatch). **B** opens it at once and holds it
open while B is down (nothing else uses a tap of B, so there is no wait); holding the right thumbstick
click for a quarter of a second does the same. Either way, letting go of the button that opened it
picks. It opens a ring of the weapons the player has, with the game's own
HUD icons, in front of the head, held still in the room while it's open. Point the weapon hand at
one (a dot shows where the hand points) or push the right stick towards it; the highlighted
weapon's name shows in the middle, and each new highlight gives a short tick on the weapon hand's
controller. Letting go switches to it with the ordinary `weapon <n>` command;
letting go in the middle ("Cancel"), or with nothing highlighted, switches nothing. The weapon in
hand has a bar under it, and weapons without enough ammo are shown crossed out on red (they can
still be picked, as with a number key). While the wheel is open the triggers don't fire (one still
held when it shuts stays dead until let go), the right stick doesn't turn (it must come back to the
middle first), and a quick tap of the click keeps its old meaning. `vr_leftHanded` points with the
left hand. It shuts without a pick when a menu or the console opens, a cutscene starts, the player
dies, the level ends, a game is saved or loaded, or the game quits.

**Slow motion with the wheel.** Only in plain single player: time runs at `vr_wheelSlowMotion` (0.2)
while the wheel is open, easing down over 150 ms and back up over 100 ms after a pick, and straight
back to normal when it shuts for any other reason. It is a factor on the single-player simulation's
clock, not a cvar, so nothing is saved. With a co-op session (host with `sp_coop` set, or a guest)
and in Holomatch, the wheel works the same but time is never touched; this is checked every frame,
so a guest joining while the wheel is open brings time back to normal at once. Holomatch sees only
the usual `weapon <n>` command.

**In menus and the console:** point the weapon hand at the menu: the menu cursor follows where the
ray meets the menu panel, and the weapon hand's trigger clicks (mouse button 1). A **red laser**
(`vr_menuLaser`, on by default) is drawn from the hand to the cursor: one thin quad layer in the
menu's frame, turned about its line to face the head, placed where the ray meets the panel's plane
(it keeps going past the panel's edge, so you can find the panel again). The right
thumbstick still moves the cursor (the ray takes the cursor back as soon as the hand moves), the
left thumbstick sends arrow keys, A and X are Enter, and B, Y and the menu button are Escape. A button held
while a menu opens or closes does nothing in the new mode until it is released. The ray works while
the menu is held in the room (`vr_menuHeadLocked 0`, the default); with it locked to the head the stick
is the only pointer.

**Simple controller (fallback):** right select = fire, left select = use (X), either menu button = menu.
There is no movement on that profile.

**Poses:** the head (OpenXR VIEW space), and per hand the aim pose (the pointing ray) and the grip
pose, are located every frame in the same space as the eye views. `VR_GetInputState()` and
`VR_GetHandPose()` (`efcode/vr/vr_input.h`) expose them (the weapon hand's aim drives the shots, the
wheel and the menu pointer).

**cvars (archived):**

| cvar | Default | Meaning |
|---|---|---|
| `vr_turnMode` | 0 | 0 = snap turn, 1 = smooth turn |
| `vr_snapTurnAngle` | 45 | degrees per snap (eased over 0.1 s) |
| `vr_turnSpeed` | 150 | smooth turn, degrees per second at full deflection |
| `vr_stickDeadzone` | 0.25 | thumbstick dead zone (0..0.9) |
| `vr_menuLaser` | 1 | 0 = no laser from the hand to the menu cursor |
| `vr_hudDistance`, `vr_hudWidth` | 1.2, 1.2 | the HUD's distance and width in metres (were 1.8 and 1.5; a config saved by an older build is reset to these once, through `vr_layoutVersion`) |
| `vr_menuDistance`, `vr_menuWidth` | 1.0, 1.3 | the menu's distance and width in metres (were 2.0 and 2.0; the same one-time reset) |
| `vr_debugFrames` | 0 | debugging: log what each frame submits; `vr_screenshot <n>` then writes n frames in a row, numbered by the same frame |
| `vr_menuCursorSpeed` | 500 | menu cursor speed from the right thumbstick, pixels per second |
| `vr_leftHanded` | 0 | 1 = the weapon in the left hand (its trigger fires and its grip is alternate fire; the other hand's trigger jumps and its grip crouches) |
| `vr_debugInput` | empty | debugging: pretend controller state, `"buttons trigL trigR gripL gripR stickX stickY"` (the buttons are the `VR_BUTTON_` bits in `vr_input.h`), added to the real controllers |
| `cg_vrLaser` | 1 | 0 = off, 1 = laser and dot at the hit point, 2 = dot only |
| `cg_vrOffHand` | 1 | draw a marker at the off hand |
| `vr_wheelSlowMotion` | 0.2 | single player: how fast time runs while the weapon wheel is open (1 = no slow motion) |
| `vr_wheelDistance`, `vr_wheelWidth`, `vr_wheelHeight` | 1.0, 1.4, −0.15 | the wheel's place: the 2D layer (HUD included) moves there while it's open, metres |
| `vr_roomScale`, `vr_leanRadius`, `vr_seated`, `vr_crouchThreshold`, `vr_moveDirection`, `vr_comfortVignette` | | head position, room-scale, crouch, move direction and comfort: see [MOVEMENT.md](MOVEMENT.md) |

Movement follows the stick's deflection and, by default, the head's yaw; turning pivots about the
head. Details in [MOVEMENT.md](MOVEMENT.md).

## Code

| Piece | Path |
|---|---|
| Actions, bindings, sync, poses (OpenXR only) | `EFAndroid-SP/app/jni/efcode/vr/vr_input.c`, `vr_input.h`, `vr_local.h` |
| Controller map onto the engine (compiled into the engine target), the eased snap turn, the menu pointer and its laser | `efcode/vr/vr_input_engine.c` (the ray onto the 2D layer: `VR_Pointer_OnCanvas` and `VR_Pointer_Laser` in `vr_wheel.c`; the laser layer: `VR_SetMenuLaser` and `VR_EndFrame` in `vr_openxr.c`) |
| Weapon wheel: pointing, drawing, slow motion (engine target) | `efcode/vr/vr_wheel.c`, `vr_wheel.h`; the single-player weapon list comes from the cgame (`CG_VR_SETWEAPONS`, `cg_vr.cpp`) through `sp/sp_bridge.cpp`, which also slows its clock |
| Session wiring | `efcode/vr/vr_openxr.c`: `VR_Input_*` calls in `VR_Init`, `VR_Shutdown`, `VR_CreateSession`, `VR_DestroySession`, `VR_PollEvents` and `VR_BeginFrame` |
| Android shell | `android/AndroidManifest-quest.xml`, `android/java-quest/` (`StartActivity`, `QuestActivity`), `android/jni/quest_jni.c`, `android/CMakeLists.txt` (`EF_VR`, and `EF_COOP` with it), `android/make-apk.sh` (`VR=1`) |
| Touch controls (flat APK only: none in the Quest build) | `efcode/client/touch_controls.h` (`TC_HiddenByVR`), `cl_scrn.c`, `sdl/sdl_input.c` |

## Known issues

From the first session on a headset (2026-11-27); the numbers are the owner's.

- **4. The pause menu flickers once when it opens.** *Fixed (vr-r3-controls), untested on a headset.* Cause,
  from a frame-by-frame capture in the simulator: the single-player menu's first two frames aren't a clean
  picture. The first is the menu on an opaque black background, and the second has nothing drawn at all (the
  bridge hides the menu for one frame to capture the save-game thumbnail). Shown as they came, the 2D layer
  flashed black and then blanked. Now, for three frames after a game frame, the world stays frozen and the 2D
  layer keeps showing the last HUD image (no new image, no dimming, no laser), and the menu then comes in whole
  (`R_VR_BeginFrame` in `renderervk/tr_vr.c`, `VR_SetHoldCanvas`). The menu is about 40 ms later (3 frames at 72 Hz).
- **7. Walking into a ladder doesn't climb it; forward plus jump does.** Fixed (round 3): the
  game's climb follows the command's pitch, which in VR is the body's (single player) or the gun's
  (Holomatch), not the head's. Moving into a ladder now climbs it, and down while looking down;
  see Ladders in [MOVEMENT.md](MOVEMENT.md) (`vr_ladderClimb`, `vr_ladderDownPitch`).
- **8. Brief white dots in the distance, for a frame or two.** Not reproduced in the simulator:
  with the head still, 12 eye captures in a row show no pixel that lights up in one frame only
  (borg1, flares on). The eye images clear to black, so cracks between surfaces would show black,
  not white. Candidates left: shimmer of small bright details as the real head moves (no
  mipmap/MSAA cure for sub-pixel highlights), and the flare visibility test, which reads its
  result one frame late while the previous frame may still be drawing. Headset checks:
  `r_flares 0`; and `vr_msaa 0` (then `vid_restart`) against the default 4.
- **10. The HUD and the menu layer may float too far away.** *Changed (vr-r3-controls), to judge on a headset.*
  The HUD is now 1.2 m away and 1.2 m wide (was 1.8 and 1.5), the menu 1.0 m and 1.3 m (was 2.0 and 2.0), and
  the weapon wheel, which was already 1.0 m, is unchanged. The cvars are archived and a saved config holds the
  old numbers, so a new default alone would never have reached the headset: a one-time reset (`vr_layoutVersion`)
  puts the four cvars on the new values. The pointer needs nothing more, because it intersects the quad as it was
  submitted. If it is still wrong, set `vr_menuDistance`, `vr_menuWidth`, `vr_hudDistance` and `vr_hudWidth`
  (keep the width about 1.2 to 1.3 times the distance for the same angular size).
- **The start screen (launcher) is untested on a headset.** Checked in an Android emulator only, at
  the Quest panel's size (1280 × 800 dp): the pages' layout (no label cut off), the name and password
  keyboards, the imports through Android's pickers (a CD image, a .bin with its .cue, a BaseEF folder,
  each a small test image), the Expansion Pack line, and the backup and restore. Not checked anywhere
  yet: the server browser (the master servers and joining), the co-op join and LAN search, and GOG's
  installer. To check on the headset: that the file and folder pickers open, the import of a real CD
  image or GOG installer, that no label is cut off, the pointer targets' size, Browse Servers (the list
  fills, a tap joins, a locked server asks for its password), and a co-op join with Munro's look,
  a character and a password. Watch `adb logcat -s AwayTeamEF`.
- **R4. Co-op: a flat partner didn't see the VR player's phaser beam where it aimed.** Fixed in
  the Meta XR Simulator, still to check on a headset: partners now draw it toward where the hand
  points (see [COOP.md](COOP.md#what-flat-partners-see)). A partner on an older 1.5.x build sees
  it end within about 20 units of the real spot, as it does for a flat player.
- **Fixed (not yet tried on a headset): Use on switches and consoles, and the co-op ping.** Use
  now picks from the hand, the head and the switch areas, and a switch that wants to be faced
  accepts the head or the hand ([MOVEMENT.md](MOVEMENT.md#how-use-picks-a-target)); the ping
  goes where the hand points ([COOP.md](COOP.md#the-ping)).
- **The start screen's step into the game is untested on a headset.** The Quest should switch from
  the flat panel to VR when `QuestActivity` starts, because it carries the VR category. If it opens
  flat instead, start the game directly with
  `adb shell am start -n org.awayteamef.quest/org.awayteamef.app.QuestActivity`.
