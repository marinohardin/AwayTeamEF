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
  the head is only a view offset. The game's own lean is not used in VR: the cgame skips its
  `leanofs` view offset, and Use + strafe no longer starts the single player key lean (the command
  gets a forward move of 1, below any speed that matters). The co-op work will drive the game's
  lean from the head later.
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

## Turning

Snap turn is the default (`vr_snapTurnAngle`, 45), smooth turn the option (`vr_turnMode 1`,
`vr_turnSpeed`). A turn pivots about the head, not the body: the head origin is rotated so the
eye stays where it is in the world. The hands use the same body yaw, so they stay with the head.

## Comfort

`vr_comfortVignette` (0..1, default 0) darkens the edges of the view while the stick moves the
player or a smooth turn runs, in proportion to how fast. It is a head-locked quad with a radial
mask, drawn by the runtime, so it costs nothing in the eye render.

## cvars

| cvar | Default | Meaning |
|---|---|---|
| `vr_roomScale` | 1 | 1: stepping in the room moves the player (single player); 0: head is a view offset only |
| `vr_leanRadius` | 0.3 | metres the head may move from the body before the body follows |
| `vr_seated` | 0 | 1: seated play, crouch with the button only |
| `vr_crouchThreshold` | 0.3 | metres below the recentred height that crouch the player (0 off) |
| `vr_moveDirection` | 0 | 0: move where the head looks; 1: where the off-hand points |
| `vr_comfortVignette` | 0 | edge darkening while moving or smooth turning, 0..1 |

Debugging (not archived): `vr_debugHead "side fwd up"` (metres) adds a pretend head offset,
`vr_debugStick "x y"` a pretend move stick, `vr_debugMove 1` (2 Hz) or `2` (10 Hz) prints the head,
body and movement state, and `vr_debugTurn <degrees>` turns as a snap would.

## Limits

- Room-scale moves the body only in single player. Holomatch and co-op guests get the lean and
  the dragged head origin, not a collision move (that needs the server).
- The flat first-person weapon is still drawn at the game eye; the hand weapon work replaces it.
- Not yet run on a headset.
