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
// the weapon wheel (the engine draws it and picks with the hand): the cgame reports, once per
// drawn frame, the weapons the viewed player may switch to, with their icons (vrWheelWeapons_t).
// The engine answers 1 while its wheel is open. Only a VR engine takes it.
#define CG_VR_SETWEAPONS	0x5654

#define VR_WHEEL_MAX_WEAPONS	16

typedef struct {
	int		count;
	int		current;							// the weapon selected now (cg.weaponSelect)
	int		locked;								// no switching now (a hypo in use, following someone)
	int		dead;								// the viewed player is dead
	int		noAmmoIcon;							// shader drawn over a weapon that's out of ammo
	int		weapon[VR_WHEEL_MAX_WEAPONS];		// weapon numbers, in the game's order
	int		icon[VR_WHEEL_MAX_WEAPONS];			// their HUD icons (shader handles)
	int		noAmmo[VR_WHEEL_MAX_WEAPONS];		// 1 = can't fire it (not enough ammo)
	char	name[VR_WHEEL_MAX_WEAPONS][32];		// what the HUD calls it
} vrWheelWeapons_t;

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
	vrGameHand_t	head;	// between the eyes as drawn, turned as the headset is
} vrGamePose_t;

// cgame: refreshed once per drawn frame (CG_DrawActiveFrame)
extern vrGamePose_t	cg_vrPose;

void		CG_VR_RefreshPose( void );
// the newest pose straight from the engine (the game's shots, between drawn frames)
qboolean	VR_FetchPose( vrGamePose_t *pose );
// a hand in world space, from an eye and the body's yaw; qfalse when untracked
qboolean	VR_HandToWorld( const vrGamePose_t *pose, int hand, const vec3_t eye, float yaw,
				vec3_t origin, vec3_t axis[3] );

// VR Use (g_utils.cpp): what Use acts on for player 1, from the weapon hand, the head's gaze and
// the use-button triggers about the player. The game's Use and the cgame's highlight both ask
// G_VRUsePick, with the eye, yaw and pose the cgame drew the frame with (vr_useView) while fresh.
#define VR_USE_NONE			0
#define VR_USE_ENTITY		1	// a usable thing (ValidUseTarget)
#define VR_USE_LOCKED		2	// a locked stasis door (the refusal sound)
#define VR_USE_NPC			3	// a crew member who answers
#define VR_USE_TRIGGER		4	// a trigger that fires on Use (most switches and consoles)
#define VR_USE_VIA_HAND		0
#define VR_USE_VIA_HEAD		1
#define VR_USE_VIA_TRIGGER	2

typedef struct {
	int		kind;			// VR_USE_
	int		entityNum;		// ENTITYNUM_NONE for nothing
	int		via;			// VR_USE_VIA_
} vrUsePick_t;

typedef struct {
	int				time;	// cg.time of the frame, 0: none yet
	vec3_t			eye;	// cg.refdef.vieworg
	float			yaw;	// the body's yaw (cg.refdefViewAngles[YAW])
	vrGamePose_t	pose;
} vrUseView_t;

extern vrUseView_t	vr_useView;

qboolean	G_VRUsePick( gentity_t *ent, const vrGamePose_t *pose, const vec3_t eye, float yaw, vrUsePick_t *pick );
// a FACING trigger in VR: 1 the hand, head or body faces movedir, 0 none does, -1 not a VR player
int			G_VRFacing( gentity_t *ent, const vec3_t movedir );
// what Use would do for player 1 now (VR_USE_), -1 when not in VR
int			G_VRUseWouldAct( gentity_t *ent );
// where a VR player points (player 1: the hand, else the head; a VR guest: its hand); qfalse: flat
qboolean	G_VRPointRay( gentity_t *ent, vec3_t start, vec3_t dir );

// cgame: tell the engine's weapon wheel what the player can switch to (once per drawn frame in VR)
void		CG_VR_ReportWeapons( void );

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
// the head's gaze and the weapon hand's pointing ray (with or without a weapon), for what the
// player can use; the box drawn round it in the world (the flat box doesn't show in VR)
qboolean	CG_VR_HeadRay( vec3_t start, vec3_t forward );
qboolean	CG_VR_PointRay( vec3_t start, vec3_t forward );
void		CG_VR_NoteUseBox( const gentity_t *ent, const vec4_t color );	// from the HUD label
void		CG_VR_AddUseBox( void );	// into the scene, once a frame
// the newest shot ray, drawn at most maxAge ms ago
qboolean	CG_VR_RecentAimRay( vec3_t start, vec3_t forward, int maxAge );

#endif
