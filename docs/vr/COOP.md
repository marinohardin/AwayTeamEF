# Quest VR: co-op with flat partners

A VR player, as host or guest, plays the campaign with a flat Mac, Windows or Linux player. Co-op
already refuses a partner on another app version, so both ends always have this code; nothing is
negotiated and the protocol version is unchanged. A flat player sends and sees exactly what it did
before unless the partner is in VR.

## The lean

- **Measured (engine, `EF_VR`):** `VR_Move_LeanButtons` (`vr_move.c`) takes the eyes' sideways
  offset from the head origin in the body frame, in game units, times `vr_leanScale` (default 1),
  with a 1.5-unit dead zone and the key lean's clamp of 32. Within `vr_leanRadius` (0.3 m) that is
  at most about 10 units.
- **Carried:** in the game command's int buttons, bit 15 `BUTTON_VRLEAN` ("a lean is present") and
  bits 16-23 the signed lean. The engine's own usercmd buttons are a byte, so the SP bridge ORs the
  bits into the game's command (host and single player) and into the guest's co-op commands. They
  are never merged into the buttons held between commands: each command carries the newest lean.
- **Applied (game, every build):** `ClientThink` takes the VR bits out of the command before
  anything else sees it (`G_TakeVRButtons`). With a lean present, `PM_VRLean` (`bg_pangles.cpp`)
  moves `leanofs` toward it by at most 8 a call, through the key lean's own trace from the eye, so a
  wall stops it. No Use or strafe, no debounce: walking while leaning works. Standing still and
  leaning 4 units or more plays the lean stance on the legs. Without the bit the key lean code runs
  as before.
- **Seen:** partners see `leanofs` and the stance through the existing replica. On the VR player's
  own screen the cgame skips the `leanofs` view offset; the real head already moved the eyes.

## The guest's hand aim

- The guest's cgame reports its weapon hand each frame (`CG_VR_SETAIM`, a VR-only cgame syscall):
  the muzzle as drawn, as an offset from the eye, and the hand's ray, both in the body frame before
  the yaw.
- The bridge sends it with each command while it is fresh (under 200 ms): bit 14 `BUTTON_VRAIM`,
  and after the command on the wire 10 more bytes (the muzzle in eighths of a unit, the ray's pitch
  and yaw). A flat guest never sets the bit, so its packets are byte for byte as before.
- The host's game gets it through the co-op bridge (`GuestVRAim`, bridge version 6) with the command
  it applies. Player 2's shot then leaves from player 2's eye plus the muzzle offset, along the ray,
  turned by player 2's body yaw, with the same eye-to-muzzle wall trace as a VR player 1
  (`W_VRHandShot`). Hitscan and projectiles both.
- The guest's own predicted shot uses the same hand ray (`coop_predict.cpp`).
- A VR host's own hand fire needs nothing new: its game reads its own headset's pose.

## What flat partners see

- The lean stance (above).
- The model's head looks, and its torso turns, where the weapon hand points
  (`CG_PlayerAngles`, `COOP_VRAimAngles`). The host knows both players' aims (its own headset, the
  guest's commands) and sends them in the replica's globals as an optional chunk (`RVRA`), which
  exists only while a VR player has a tracked weapon hand. The third-person weapon follows the torso.
- A flat player's model is drawn as before.

## cvars

| cvar | Default | Meaning |
|---|---|---|
| `vr_leanScale` | 1 | how much of the head's sideways offset becomes the game's lean (0: none) |
| `coop_vrDebug` | 0 | host: print each shot player 2 fires from a VR hand ("VR shot (player 2)") |

`vr_debugMove 3` prints the lean sent to the game twice a second.

## Code

| Piece | Path |
|---|---|
| Lean measurement | `efcode/vr/vr_move.c` (`VR_Move_LeanButtons`) |
| Bits into the game command, the aim syscall, the guest's aim | `sp/sp_bridge.cpp` (`EF_VR`) |
| The aim on the wire, lean bits kept out of held buttons | `efcode/sys/sys_coop_net.c` |
| Bits taken out, the lean | `game/g_active.cpp`, `game/bg_pangles.cpp` |
| Player 2's hand shot | `game/g_weapon.cpp`, `coop/coop_vr.cpp` |
| Aim for drawing | `coop/coop_vr.cpp`, `coop/coop_replica.cpp`, `cgame/cg_players.cpp` |
| The guest's report and predicted shot | `cgame/cg_vr.cpp`, `coop/coop_predict.cpp` |
