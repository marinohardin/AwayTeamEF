// vr_shared.h: the VR hand poses, for the cgame and the game (single player).
//
// The VR build's engine hands the tracked controller poses to the module through one cgame
// syscall, CG_VR_GETPOSE. Any other engine returns 0 for it (an unknown syscall), so with no
// VR running everything here stays inactive and every caller takes its usual path.
//
// The poses are relative to the player, not the world: each hand's origin is an offset from the
// game's eye (the view origin before the headset's own offset), in game units, in the body frame
// (+X forward, +Y left, +Z up) before the body's yaw. The cgame and the game each turn that into
// world space with their own eye and yaw (VR_HandToWorld), so the view weapon and the shot agree.
//
// Not saved, not sent anywhere: the Holomatch modules and the network protocol never see it.

#ifndef VR_SHARED_H
#define VR_SHARED_H

// outside cgameImport_t on purpose: a number no other syscall will take
#define CG_VR_GETPOSE		0x5652
// co-op guest: the weapon hand for the next commands, float[6]: the muzzle from the eye and the
// hand's ray, in the body frame before the yaw (only a VR engine's co-op guest uses it)
#define CG_VR_SETAIM		0x5653

#define VR_HAND_WEAPON		0	// the dominant hand (the engine's vr_leftHanded picks it)
#define VR_HAND_OFFHAND		1
#define VR_NUM_HANDS		2

typedef struct {
	int		valid;			// tracked this frame
	float	origin[3];		// from the game's eye, body frame, game units
	float	axis[3][3];		// forward (the pointing ray), left, up; body frame
} vrGameHand_t;

typedef struct {
	int				active;	// VR frames go to the headset (the hands may still be untracked)
	vrGameHand_t	hand[VR_NUM_HANDS];
	vrGameHand_t	head;	// between the eyes as drawn (origin; the axes only roughly)
} vrGamePose_t;

// cgame: refreshed once per drawn frame (CG_DrawActiveFrame)
extern vrGamePose_t	cg_vrPose;

void		CG_VR_RefreshPose( void );
// the newest pose straight from the engine (the game's shots, between drawn frames)
qboolean	VR_FetchPose( vrGamePose_t *pose );
// a hand in world space, from an eye and the body's yaw; qfalse when untracked
qboolean	VR_HandToWorld( const vrGamePose_t *pose, int hand, const vec3_t eye, float yaw,
				vec3_t origin, vec3_t axis[3] );

// cg_vrGunInfo 2: log the shots' rays (testing)
qboolean	VR_DebugShots( void );

// cgame: the VR view weapon (cg_vr.cpp). qtrue = hand and gun are placed (no bob, no depth hack)
qboolean	CG_VR_PlaceViewWeapon( int weaponNum, qhandle_t handsModel, refEntity_t *hand );
// the shot's ray for the local player: the muzzle (pulled back to the eye's side of walls) and
// the hand's forward. qfalse when the weapon isn't in a tracked hand.
qboolean	CG_VR_ShotRay( const vec3_t muzzle, vec3_t start, vec3_t forward );
// the laser or dot at the real hit point, and the off-hand marker (after the weapon is placed)
void		CG_VR_AddAimHelpers( const vec3_t muzzle );
qboolean	CG_VR_HideCrosshair( void );
// this frame's shot ray (set by CG_VR_AddAimHelpers), for the crosshair-entity scan
qboolean	CG_VR_AimRay( vec3_t start, vec3_t forward );
// the newest shot ray, drawn at most maxAge ms ago
qboolean	CG_VR_RecentAimRay( vec3_t start, vec3_t forward, int maxAge );

#endif
