# Quest VR: head position, room-scale and stick movement

Everything here is compiled only with `EF_VR` and acts only while the headset is drawing
(`vr_active 1`). Flat builds and flat players are unchanged.

Code: `EFAndroid-SP/app/jni/efcode/vr/vr_move.c` and `vr_move.h`, with small hooks in
`vr_input_engine.c`, `client/cl_input.c`, `sp/sp_bridge.cpp`, `vr_openxr.c`, `renderervk/tr_vr.c`,
`renderervk/vk.c` and the cgame's `cg_view.cpp`.

## The shared convention

Eyes, hands and movement all place a tracked pose in the world the same way:

    world = body origin + R(body yaw) * ( pose - VR_GetHeadOrigin() ) * vr_worldScale

- Poses are in the session's reference space (OpenXR axes: +X right, +Y up, -Z forward). In the
  game frame forward = -Z, left = -X, up = +Y.
- The body yaw is the game's view yaw (`cl.viewangles[YAW]` plus the delta angles). Turning,
  snap or smooth, changes only that yaw.
- The head origin is the point in the room over the player's origin, at standing eye height. A
  recentre puts it under the head again: hold the left thumbstick click (0.7 s; a tap is still
  the previous weapon), the `vr_recenter` command, or the headset's own recentre.

## Head position

- **Lean.** Within `vr_leanRadius` (0.3 m; capped so the eye stays inside the 16-unit player box)
  the head is a view offset. The head also drives the game's lean (`leanofs`) as an analog value:
  each command carries the head's sideways offset in game units (times `vr_leanScale`) and the game
  moves `leanofs` toward it through the key lean's own wall trace. NPCs and co-op partners see that
  lean (see [COOP.md](COOP.md)). On the VR player's own screen the cgame skips the `leanofs` view
  offset, since the real head already moved the eyes. The key lean (Use + strafe) is off in VR.
- **Room-scale (single player).** Past the radius the body follows the head. Before each game tick
  the SP bridge moves the player's box by the excess with the game's own trace
  (`MASK_PLAYERSOLID`, a stair step up to `STEPSIZE`) and the head origin moves by what the move
  achieved. `vr_roomScale 0` keeps the head as a view offset only. Without the SP bridge (Holomatch,
  a co-op guest) the head origin is dragged along instead, so the view never leaves the radius.
- **Walls.** When the body is blocked the head can still go on. A small box trace from the body's
  eye to the head pushes the eye back to the wall, and the view fades to black from 3 cm into the
  wall (black 12 cm deeper), on the dimmer layer.
- **Smoothness.** The game ticks at its own rate; the head origin used for drawing is interpolated
  from per-tick records at the cgame's own time, so the body's steps and the head line up.
- **Crouch.** Standing (`vr_seated 0`): the head going `vr_crouchThreshold` (0.3 m) below the
  recentred height crouches the player (5 cm hysteresis). The B button crouches in either mode and
  eases the view down by the game's own crouch drop. In VR the cgame keeps the eye at standing
  height, so a real crouch is never added twice.

## Stick movement

- Speed follows deflection: a round dead zone (`vr_stickDeadzone`), and the rest of the throw scales
  the usercmd from 0 to 127 (running) or 64 (`cl_run 0`). Below the walk speed the walk button is
  set, so footsteps and animation match. The usercmd layout is unchanged.
- Direction: `vr_moveDirection` 0 = where the head looks, 1 = where the off-hand (left) controller
  points.
- Measured in single player (Meta XR Simulator): about 80, 150 and 225 units/s at 0.33, 0.67 and
  full effective deflection running, and about 115 units/s at full deflection walking.

## Ladders

Both games move a player on a ladder like a swimmer: the wish is the command's forward move along
the view's forward, pitch included, plus the up move. A flat player walks into a ladder looking up
(or down) and climbs. In VR the command's pitch is not where the player looks: single player sends
the body's level pitch, Holomatch the gun hand's aim. So pushing the stick into a ladder only
pressed against it, and climbing needed jump.

`VR_Move_LadderCmd` (`vr_move.c`, called from `CL_CreateCmd` after `VR_HM_Cmd`, so the angles are
final) fixes that in the command, in every mode:

- **On a ladder?** The games' own test, on the engine's copy of the map: ladder contents at the
  feet and ladder or water at the waist (`waterlevel` 2), at the newest snapshot's origin (single
  player and co-op: the SP bridge's snapshot, `SP_VR_PlayerState`; Holomatch: `cl.snap`).
- **Into it?** The move (as walking would make it, pitch left out) must run the box into a wall
  within 24 units, heading into it within 60 degrees. Moving along the wall or away from it is
  left to the game, so stepping off sideways or walking away at the foot works as before.
- **Up or down.** Up, unless the head (the off-hand with `vr_moveDirection 1`) looks more than
  `vr_ladderDownPitch` (25) degrees below level; a 15-degree ramp in between, so it never flips
  at one angle. At the foot, looking down doesn't push down.
- **The command.** The wish is mostly the climb plus a quarter of the move into the ladder (that
  steps off at the top), at the stick's speed. It is written back as forward, right and up moves
  that the game's own sum turns into that wish for the command's pitch. The up move never goes
  negative (that would crouch): climbing down in single player sends a pitch for that command
  instead (the eyes take only the yaw from the game). In Holomatch the angles stay the gun's aim
  (H1), so there climbing down needs the gun pointed down too, or the crouch button.
- A held crouch or jump is left alone (crouch climbs down by itself). `vr_ladderClimb 0` turns
  it all off. The usercmd layout, the protocol and the games are unchanged.

## Turning

Snap turn is the default (`vr_snapTurnAngle`, 45), smooth turn the option (`vr_turnMode 1`,
`vr_turnSpeed`). A turn pivots about the head, not the body: the head origin is rotated so the
eye stays where it is in the world. The hands use the same body yaw, so they stay with the head.

## Comfort

`vr_comfortVignette` (0..1, default 0) darkens the edges of the view while the stick moves the
player or a smooth turn runs, in proportion to how fast. It is a head-locked quad with a radial
mask, drawn by the runtime, so it costs nothing in the eye render.

## How Use picks a target

Single player and the co-op host's own player (`G_VRUsePick`, `game/g_utils.cpp`). The game's
Use and the highlight both call it, with the eye, body yaw and hand poses the frame was drawn with,
so a press uses what is boxed. In order:

1. What the weapon hand points at, up to 88 units from the hand: a thin ray first, then a ray
   6 units thick for small buttons.
2. The same along the head's gaze, up to 80 units from the eyes.
3. A switch or console that fires on Use (a use-button trigger, which is how most of the
   campaign's switches work). It counts if the player stands in its area, or if the hand's ray or
   the gaze reaches into it from within 24 units. When the switch needs the player to face it,
   the body's view, the head or the hand may face it (within about 60 degrees, judged level).
   The flat game takes this only from the body's view, which in VR follows the stick turns, not
   the real head. That is why switches used to need odd angles, above all when the player had
   turned in the room instead of with the stick.

What Use will act on gets corner brackets in the world, drawn like a scanned thing's label: the
thing's own colour (amber when it has none), the same thickness. For a switch area the brackets go
round the switch or console it belongs to, not the invisible area in front of it: the smallest
drawn thing (not a person, not another trigger, smaller than the area, so not the lift it works)
that the area touches or comes within 8 units of. When the console is part of the level's
geometry, a small box marks where the area faces the wall. This is only the drawing; Use still
acts on the area. Without tracked hands or head, Use is the flat one. A co-op guest's Use is still the
flat one, judged on the host, except that its hand can face a switch. `cg_vrGunInfo 2` prints each
press ("VR use: ...").

## cvars

| cvar | Default | Meaning |
|---|---|---|
| `vr_roomScale` | 1 | 1: stepping in the room moves the player (single player); 0: head is a view offset only |
| `vr_leanRadius` | 0.3 | metres the head may move from the body before the body follows |
| `vr_seated` | 0 | 1: seated play, crouch with the button only |
| `vr_crouchThreshold` | 0.3 | metres below the recentred height that crouch the player (0 off) |
| `vr_moveDirection` | 0 | 0: move where the head looks; 1: where the off-hand points |
| `vr_comfortVignette` | 0 | edge darkening while moving or smooth turning, 0..1 |
| `vr_leanScale` | 1 | how much of the head's sideways offset becomes the game's lean (0: none) |
| `vr_ladderClimb` | 1 | moving into a ladder climbs it (0: the game's own ladder move only) |
| `vr_ladderDownPitch` | 25 | degrees below level the head (or off-hand) looks to climb down instead of up |

Debugging (not archived): `vr_debugHead "side fwd up"` (metres) adds a pretend head offset,
`vr_debugStick "x y"` a pretend move stick, `vr_debugMove 1` (2 Hz) or `2` (10 Hz) prints the head,
body and movement state (`3`: also the lean sent to the game), and `vr_debugTurn <degrees>` turns as a snap would.

## Limits

- Room-scale moves the body only in single player. Holomatch and co-op guests get the lean and
  the dragged head origin, not a collision move (that needs the server).
- The flat first-person weapon is still drawn at the game eye; the hand weapon work replaces it.
- Not yet run on a headset.
