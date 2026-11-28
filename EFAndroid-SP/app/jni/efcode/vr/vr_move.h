/*
===========================================================================
AwayTeamEF VR: head position, room-scale movement, analog movement and crouch (EF_VR only).

This file is part of AwayTeamEF and is licensed under the GNU General
Public License, version 2 or (at your option) any later version.
===========================================================================

vr_move.c is compiled into the engine target (an INTERFACE source of ef_vr, like
vr_input_engine.c). docs/vr/MOVEMENT.md explains the design.

The convention every VR piece shares (the eyes, the hands, the movement):

  world = body origin + R(body yaw) * ( pose - VR_GetHeadOrigin() ) * vr_worldScale

  - poses are in the session's reference space (OpenXR axes: +X right, +Y up, -Z forward)
  - the body yaw is the game's view yaw (cl.viewangles[YAW] plus the game's delta angles);
    turning (snap or smooth) changes only that yaw
  - the head origin is the point in the room that sits over the player's origin, at the
    standing eye height. Room-scale movement shifts it as the body follows the head.

Who calls what:
  vr_input_engine.c  VR_Move_Frame (every sync, render thread), VR_Move_CrouchButton,
                     VR_Move_Turning
  cl_input.c         VR_Move_Cmd (CL_CreateCmd: analog move, crouch, no double lean)
  sp_bridge.cpp      VR_Move_SPWant / VR_Move_SPMoved (each game tick, before ClientThink),
                     VR_Move_SPRecord (after each tick's snapshot), VR_Move_SPView (before the
                     cgame draws: the visible head origin, and the head-in-wall check)
*/
#ifndef EF_VR_MOVE_H
#define EF_VR_MOVE_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef EF_VR_MOVE_NO_CMD
struct usercmd_s;
// add the VR movement to a usercmd: proportional stick speed, real or button crouch, and no
// key lean from Use + strafe (the head is the lean in VR)
void	VR_Move_Cmd( struct usercmd_s *cmd );
// on a ladder, moving into it climbs (vr_ladderClimb): after the angles are final (VR_HM_Cmd)
void	VR_Move_LadderCmd( struct usercmd_s *cmd );
#endif

// once per input sync (vr_input_engine.c). inState is a const vrInputState_t *; game = no key
// catcher (stick and buttons go to the game); dt in seconds
void	VR_Move_Frame( const void *inState, int game, float dt );
void	VR_Move_CrouchButton( int down );
// the analog lean for the SP game's command: BUTTON_VRLEAN plus the head's sideways offset in game
// units (times vr_leanScale, within +-32) in bits 16-23, to OR into the command's int buttons (the
// SP bridge; the engine's usercmd buttons are a byte). 0 without a tracked head in play.
int		VR_Move_LeanButtons( void );
// a turn of the body yaw by deltaYaw degrees (counter-clockwise) this frame, and how fast,
// 0..1 of full smooth-turn speed (for the comfort vignette; a snap turn is 0)
void	VR_Move_Turned( float deltaYaw, float amount );

// single player, from the SP bridge (engine side; the game modules are unchanged)
// Room-scale: how far the body should move this tick to follow the head, in world units
// (z = 0). bodyYaw in degrees (the player's view yaw). Returns 0 when there is nothing to do.
int		VR_Move_SPWant( float bodyYaw, float want[3] );
// what the collision move achieved (world units); canMove = 0 when the game holds the player
// (cutscene, dead, frozen): the head origin then follows the head without moving the body
void	VR_Move_SPMoved( const float want[3], const float got[3], int canMove );
// after each tick: the snapshot's player origin and view height at that level time
void	VR_Move_SPRecord( int levelTime, const float origin[3], int viewHeight );
// before the cgame draws at cgTime: set the head origin that matches the interpolated body,
// then push the eye back out of walls (trace: from start to end with a small box; returns the
// fraction) and set the fade. viewYaw = the yaw the cgame draws with (degrees);
// standViewHeight = the game's standing eye above the origin.
typedef float ( *vrMoveTrace_t )( const float start[3], const float end[3], float endpos[3] );
void	VR_Move_SPView( int cgTime, float viewYaw, int standViewHeight, vrMoveTrace_t trace );

#ifdef __cplusplus
}
#endif

#endif // EF_VR_MOVE_H
