/*
===========================================================================
AwayTeamEF VR: controller input (EF_VR only).

This file is part of AwayTeamEF and is licensed under the GNU General
Public License, version 2 or (at your option) any later version.
===========================================================================

Touch controllers through OpenXR actions (vr_input.c), handed to the engine once per
frame (vr_input_engine.c), which turns them into the same commands, key events and
joystick axes a keyboard, mouse or gamepad makes. The game modules see nothing new.

  vr_input.c         action set, actions, bindings, xrSyncActions, pose spaces.
                     Knows OpenXR, not the engine. Driven by vr_openxr.c.
  vr_input_engine.c  the controller map (docs/vr/QUEST.md). Compiled into the engine
                     target itself (an INTERFACE source of ef_vr), so it can use the
                     client's own headers; vr_input.c calls VR_EngineInput() after
                     every sync.

Poses are in the session's reference space (the same space as the eye views), in OpenXR's
convention: metres, +X right, +Y up, -Z forward.
*/
#ifndef EF_VR_INPUT_H
#define EF_VR_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
	VR_HAND_LEFT,
	VR_HAND_RIGHT,
	VR_HANDS
};

typedef struct {
	float	position[3];
	float	orientation[4];		// quaternion x, y, z, w
	int		valid;				// position and orientation both tracked this frame
} vrPose_t;

// digital actions (vrInputState_t.buttons), named for the physical button; the controller map
// onto the game is in vr_input_engine.c, the default bindings in vr_input.c
#define VR_BUTTON_A			0x0001	// A: use (Holomatch: the holdable item), skips a cutscene
#define VR_BUTTON_B			0x0002	// B: the weapon wheel while held
#define VR_BUTTON_X			0x0004	// X: use
#define VR_BUTTON_MENU		0x0008	// the left menu button
#define VR_BUTTON_Y			0x0040	// Y: the mission objectives (Holomatch: the scores and its objectives)
#define VR_BUTTON_RSTICK	0x0010	// right thumbstick click: the weapon wheel
#define VR_BUTTON_LSTICK	0x0020	// left thumbstick click: recentre (held)

typedef struct {
	int			active;			// synced this frame with the session focused; else all zero
	unsigned	frame;			// counts syncs
	float		move[2];		// left thumbstick: x right, y forward (-1..1)
	float		turn[2];		// right thumbstick: x right, y up (-1..1)
	float		trigger[VR_HANDS];	// triggers (0..1): the weapon hand's fires, the other one jumps
	float		squeeze[VR_HANDS];	// grips (0..1): the weapon hand's is alternate fire, the other one crouches
	unsigned	buttons;		// VR_BUTTON_*
	vrPose_t	head;			// the VIEW space: between the eyes
	vrPose_t	aim[VR_HANDS];	// pointing ray: origin and -Z along the controller
	vrPose_t	grip[VR_HANDS];	// where the hand holds the controller
} vrInputState_t;

// the newest state (zeroed when VR is off or the session isn't focused)
const vrInputState_t *VR_GetInputState( void );
// one hand's poses; returns aim->valid (either pointer may be NULL)
int			VR_GetHandPose( int hand, vrPose_t *aim, vrPose_t *grip );
// a short vibration of one controller (amplitude 0..1); nothing without a session
void		VR_Haptic( int hand, float amplitude, float seconds );
// the interaction profile the runtime picked for a hand, e.g.
// "/interaction_profiles/oculus/touch_controller", or "" before the first one
const char *VR_GetInteractionProfile( int hand );

// the head origin all VR poses are measured from (vr.h; moved by vr_move.c), reference space, metres
void		VR_GetHeadOrigin( float origin[3] );

// a hand pose for the game modules (vr_input_engine.c), in the game's terms: relative to the
// game's eye (the point the eye offsets are added to), in game units (vr_worldScale), in the
// body frame (+X forward, +Y left, +Z up, before the body's yaw is applied)
typedef struct {
	int		valid;
	float	origin[3];		// the aim pose's position
	float	axis[3][3];		// forward (the pointing ray), left, up
} vrBodyHand_t;

#define VR_BODY_WEAPON	0	// the dominant hand (right, or left with vr_leftHanded 1)
#define VR_BODY_OFFHAND	1
#define VR_BODY_HEAD	2	// between the eyes as drawn (origin only; the axes are the head's)
// returns valid: VR frames go to the headset and the hand is tracked (or vr_debugHand drives it)
int			VR_EngineBodyHand( int which, vrBodyHand_t *out );

// provided by the engine (vr_input_engine.c): called right after every sync, on the
// render thread (inside RE_BeginFrame; VR runs with r_smp 0)
void		VR_EngineInput( const vrInputState_t *in );

#ifdef __cplusplus
}
#endif

#endif // EF_VR_INPUT_H
