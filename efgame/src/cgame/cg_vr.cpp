// cg_vr.cpp: the VR hands in single player (see game/vr_shared.h).
//
// With VR running, the first-person weapon is drawn at the weapon hand's controller and turned
// with it (roll included), shots leave from the hand along its pointing ray, a laser or dot shows
// the real hit point, and the screen-centre crosshair is hidden. Without VR (any flat engine,
// or VR switched off) cg_vrPose stays inactive and none of this runs.

#include "cg_local.h"
#include "FX_Public.h"
#include "vr_shared.h"
#include "../game/coop_public.h"

extern int (*syscall)( int arg, ... );
qboolean CG_WeaponSelectable( int i );	// cg_weapons.cpp

vrGamePose_t	cg_vrPose;

static qboolean	cg_vrCvarsRegistered;
static vmCvar_t	cg_vrLaser;			// 0 off, 1 laser and dot, 2 dot only
static vmCvar_t	cg_vrOffHand;		// 1 = a small marker at the off hand
static vmCvar_t	cg_vrGunAdjust;		// live tuning of the held weapon: "fwd left up pitch yaw roll"
static vmCvar_t	cg_vrGunInfo;		// 1 = print the held weapon's placement once per change
static vmCvar_t	vr_leftHanded;		// the engine's

static void CG_VR_RegisterCvars( void ) {
	cgi_Cvar_Register( &cg_vrLaser, "cg_vrLaser", "1", CVAR_ARCHIVE );
	cgi_Cvar_Register( &cg_vrOffHand, "cg_vrOffHand", "1", CVAR_ARCHIVE );
	cgi_Cvar_Register( &cg_vrGunAdjust, "cg_vrGunAdjust", "", 0 );
	cgi_Cvar_Register( &cg_vrGunInfo, "cg_vrGunInfo", "0", 0 );
	cgi_Cvar_Register( &vr_leftHanded, "vr_leftHanded", "0", CVAR_ARCHIVE );
	cg_vrCvarsRegistered = qtrue;
}

qboolean VR_FetchPose( vrGamePose_t *pose ) {
	if ( syscall == (int (*)( int, ... ))-1 || !syscall( CG_VR_GETPOSE, pose ) || !pose->active ) {
		memset( pose, 0, sizeof( *pose ) );
		return qfalse;
	}
	return qtrue;
}

void CG_VR_RefreshPose( void ) {
	if ( !VR_FetchPose( &cg_vrPose ) ) {
		return;
	}
	if ( !cg_vrCvarsRegistered ) {
		CG_VR_RegisterCvars();
	}
	cgi_Cvar_Update( &cg_vrLaser );
	cgi_Cvar_Update( &cg_vrOffHand );
	cgi_Cvar_Update( &cg_vrGunAdjust );
	cgi_Cvar_Update( &cg_vrGunInfo );
	cgi_Cvar_Update( &vr_leftHanded );
	CG_VR_ReportWeapons();
}

/*
The weapon wheel is the engine's (efcode/vr/vr_wheel.c): it is drawn on the 2D layer and picks
with the hand in every mode, Holomatch included. Single player tells it here what the player
may switch to, the same weapons weapnext steps through, with their HUD icons. A pick comes
back as an ordinary "weapon <n>" command.
*/
void CG_VR_ReportWeapons( void ) {
	vrWheelWeapons_t w;
	int i;

	if ( !cg.snap ) {
		return;
	}
	memset( &w, 0, sizeof( w ) );
	w.current = cg.weaponSelect;
	w.locked = ( cg.snap->ps.pm_flags & PMF_FOLLOW ) || cg.weaponSelect == WP_BLUE_HYPO || cg.weaponSelect == WP_RED_HYPO;
	w.dead = cg.snap->ps.stats[STAT_HEALTH] <= 0;
	w.noAmmoIcon = cgs.media.noammoShader;
	for ( i = FIRST_WEAPON; i <= MAX_PLAYER_WEAPONS && w.count < VR_WHEEL_MAX_WEAPONS; i++ ) {
		if ( !( cg.snap->ps.stats[STAT_WEAPONS] & ( 1 << i ) ) ) {
			continue;
		}
		w.weapon[w.count] = i;
		w.icon[w.count] = weaponData[i].weaponIcon[0] ? cgi_R_RegisterShaderNoMip( weaponData[i].weaponIcon ) : 0;
		w.noAmmo[w.count] = !CG_WeaponSelectable( i );
		if ( cg_weapons[i].item && cg_weapons[i].item->pickup_name && cg_weapons[i].item->pickup_name[0] ) {
			Q_strncpyz( w.name[w.count], cg_weapons[i].item->pickup_name, sizeof( w.name[0] ) );
		} else if ( i == WP_IMOD ) {
			Q_strncpyz( w.name[w.count], "I-MOD", sizeof( w.name[0] ) );	// its item has no name
		}
		w.count++;
	}
	syscall( CG_VR_SETWEAPONS, &w );
}

// body frame -> world: turn by the body's yaw, then add to the eye
qboolean VR_HandToWorld( const vrGamePose_t *pose, int hand, const vec3_t eye, float yaw,
	vec3_t origin, vec3_t axis[3] ) {
	const vrGameHand_t *h;
	float c, s;
	int i;

	if ( !pose->active || hand < 0 || hand >= VR_NUM_HANDS || !pose->hand[hand].valid ) {
		return qfalse;
	}
	h = &pose->hand[hand];
	c = cos( DEG2RAD( yaw ) );
	s = sin( DEG2RAD( yaw ) );
	origin[0] = eye[0] + c * h->origin[0] - s * h->origin[1];
	origin[1] = eye[1] + s * h->origin[0] + c * h->origin[1];
	origin[2] = eye[2] + h->origin[2];
	for ( i = 0; i < 3; i++ ) {
		axis[i][0] = c * h->axis[i][0] - s * h->axis[i][1];
		axis[i][1] = s * h->axis[i][0] + c * h->axis[i][1];
		axis[i][2] = h->axis[i][2];
	}
	return qtrue;
}

/*
==============================================================================

WHERE EACH WEAPON SITS IN THE HAND

The first-person models are built for a camera at the hand frame's origin (the flat view puts
that frame at the eye). In VR the hand frame is put where the controller is, so that the
weapon's grip lands in the player's hand:

  grip    the point of the model, in the hand frame (forward, left, up), that the controller
          holds. The default comes from the model (behind and below the middle of its bounds,
          through the hands model's tag_weapon); the table adds to it.
  angles  pitch, yaw and roll that line the barrel up with the controller's pointing ray,
          turned about the grip.

cg_vrGunAdjust "fwd left up pitch yaw roll" adds to the held weapon, for tuning in the headset;
cg_vrGunInfo 1 prints the result.

==============================================================================
*/

typedef struct {
	float	grip[3];		// added to the model's default grip
	float	angles[3];		// pitch, yaw, roll
} vrWeaponFit_t;

static const vrWeaponFit_t vr_weaponFit[WP_NUM_WEAPONS] = {
	{ { 0, 0, 0 }, { 0, 0, 0 } },	// WP_NONE
	{ { 0, 0, 0 }, { 0, 0, 0 } },	// WP_PHASER
	{ { 0, 0, 0 }, { 0, 0, 0 } },	// WP_COMPRESSION_RIFLE
	{ { 0, 0, 0 }, { 0, 0, 0 } },	// WP_IMOD
	{ { 0, 0, 0 }, { 0, 0, 0 } },	// WP_SCAVENGER_RIFLE
	{ { 0, 0, 0 }, { 0, 0, 0 } },	// WP_STASIS
	{ { 0, 0, 0 }, { 0, 0, 0 } },	// WP_GRENADE_LAUNCHER
	{ { 0, 0, 0 }, { 0, 0, 0 } },	// WP_TETRION_DISRUPTOR
	{ { 0, 0, 0 }, { 0, 0, 0 } },	// WP_QUANTUM_BURST
	{ { 0, 0, 0 }, { 0, 0, 0 } },	// WP_DREADNOUGHT
	{ { 0, 0, 0 }, { 0, 0, 0 } },	// WP_PROTON_GUN
	{ { 0, -6, 14 }, { 0, 0, 0 } },	// WP_TRICORDER: the model has the forearm; held up in front
	{ { 0, -6, 14 }, { 0, 0, 0 } },	// WP_VOYAGER_HYPO (as the tricorder; not checked)
	{ { 0, -6, 14 }, { 0, 0, 0 } },	// WP_BLUE_HYPO (not checked)
	{ { 0, -6, 14 }, { 0, 0, 0 } },	// WP_RED_HYPO (not checked)
	// the rest are enemy weapons: the default grip
};

// the model's default grip, in the hand frame (cached per weapon)
static qboolean	vr_gripKnown[WP_NUM_WEAPONS];
static vec3_t	vr_gripDefault[WP_NUM_WEAPONS];

static void CG_VR_DefaultGrip( int weaponNum, qhandle_t handsModel, vec3_t grip ) {
	const weaponInfo_t *weapon = &cg_weapons[weaponNum];
	orientation_t tag;
	vec3_t mins, maxs, mid, size;
	int i;

	if ( vr_gripKnown[weaponNum] ) {
		VectorCopy( vr_gripDefault[weaponNum], grip );
		return;
	}
	cgi_R_ModelBounds( weapon->weaponModel, mins, maxs );
	for ( i = 0; i < 3; i++ ) {
		size[i] = maxs[i] - mins[i];
		mid[i] = mins[i] + 0.5f * size[i];
	}
	// behind and below the middle: where a hand holds a pistol grip
	mid[0] -= 0.25f * size[0];
	mid[2] -= 0.25f * size[2];

	// model space -> the hand frame, through the hands model's tag (its first frame)
	memset( &tag, 0, sizeof( tag ) );
	cgi_R_LerpTag( &tag, handsModel, 0, 0, 0, "tag_weapon" );
	VectorCopy( tag.origin, grip );
	for ( i = 0; i < 3; i++ ) {
		VectorMA( grip, mid[i], tag.axis[i], grip );
	}
	VectorCopy( grip, vr_gripDefault[weaponNum] );
	vr_gripKnown[weaponNum] = qtrue;
}

qboolean CG_VR_PlaceViewWeapon( int weaponNum, qhandle_t handsModel, refEntity_t *hand ) {
	static int lastInfoWeapon = -1;
	vec3_t org, ctrl[3], adjAxis[3], grip, angles;
	float tune[6] = { 0, 0, 0, 0, 0, 0 };
	int i;

	if ( !VR_HandToWorld( &cg_vrPose, VR_HAND_WEAPON, cg.refdef.vieworg, cg.refdefViewAngles[YAW], org, ctrl ) ) {
		return qfalse;
	}

	CG_VR_DefaultGrip( weaponNum, handsModel, grip );
	VectorClear( angles );
	if ( weaponNum > WP_NONE && weaponNum <= WP_RED_HYPO ) {
		VectorAdd( grip, vr_weaponFit[weaponNum].grip, grip );
		VectorCopy( vr_weaponFit[weaponNum].angles, angles );
	}
	if ( cg_vrGunAdjust.string[0] ) {
		sscanf( cg_vrGunAdjust.string, "%f %f %f %f %f %f", &tune[0], &tune[1], &tune[2], &tune[3], &tune[4], &tune[5] );
		for ( i = 0; i < 3; i++ ) {
			grip[i] += tune[i];
			angles[i] += tune[3 + i];
		}
	}
	if ( vr_leftHanded.integer ) {
		// no mirrored models: the same model, held from the other side
		grip[1] = -grip[1];
		angles[YAW] = -angles[YAW];
		angles[ROLL] = -angles[ROLL];
	}

	// the hand frame: the controller's, turned by the fit angles, moved so the grip is in the hand
	AnglesToAxis( angles, adjAxis );
	MatrixMultiply( adjAxis, ctrl, hand->axis );
	VectorCopy( org, hand->origin );
	for ( i = 0; i < 3; i++ ) {
		VectorMA( hand->origin, -grip[i], hand->axis[i], hand->origin );
	}

	if ( cg_vrGunInfo.integer && lastInfoWeapon != weaponNum ) {
		CG_Printf( "VR weapon %d: grip %.1f %.1f %.1f, angles %.1f %.1f %.1f\n", weaponNum,
			grip[0], grip[1], grip[2], angles[0], angles[1], angles[2] );
		lastInfoWeapon = weaponNum;
	} else if ( !cg_vrGunInfo.integer ) {
		lastInfoWeapon = -1;
	}
	return qtrue;
}

/*
==============================================================================

THE SHOT'S RAY, THE LASER, THE OFF HAND

==============================================================================
*/

// eye-to-hand: a muzzle pushed through a wall comes back to this side of it
static void CG_VR_PullMuzzle( const vec3_t muzzle, vec3_t start ) {
	trace_t tr;
	vec3_t dir;

	CG_Trace( &tr, cg.refdef.vieworg, NULL, NULL, muzzle, cg.snap->ps.clientNum, MASK_SHOT );
	if ( tr.startsolid || tr.allsolid ) {
		VectorCopy( cg.refdef.vieworg, start );
		return;
	}
	VectorCopy( tr.endpos, start );
	if ( tr.fraction < 1.0f ) {
		VectorSubtract( muzzle, cg.refdef.vieworg, dir );
		VectorNormalize( dir );
		VectorMA( start, -1.0f, dir, start );
	}
}

qboolean CG_VR_ShotRay( const vec3_t muzzle, vec3_t start, vec3_t forward ) {
	vec3_t org, axis[3];

	if ( !VR_HandToWorld( &cg_vrPose, VR_HAND_WEAPON, cg.refdef.vieworg, cg.refdefViewAngles[YAW], org, axis ) ) {
		return qfalse;
	}
	CG_VR_PullMuzzle( muzzle, start );
	VectorCopy( axis[0], forward );
	return qtrue;
}

qboolean VR_DebugShots( void ) {
	return cg_vrPose.active && cg_vrGunInfo.integer == 2;
}

qboolean CG_VR_HideCrosshair( void ) {
	return cg_vrPose.active ? qtrue : qfalse;
}

// the eyes' midpoint in the world (a billboard has to face the real head, not the game's eye)
static void CG_VR_HeadWorld( vec3_t head ) {
	vec3_t axis[3];

	if ( !cg_vrPose.head.valid ) {
		VectorCopy( cg.refdef.vieworg, head );
		return;
	}
	vrGamePose_t p = cg_vrPose;
	p.hand[0] = p.head;	// VR_HandToWorld places entry 0
	p.hand[0].valid = 1;
	VR_HandToWorld( &p, 0, cg.refdef.vieworg, cg.refdefViewAngles[YAW], head, axis );
}

/*
==============================================================================

THE VIEW EFFECTS FACE

The effects code builds its sprites, particles and beams as polygons turned to the view, and
skips those behind it. In VR the eyes look where the head looks, not along the body's flat view,
so those would show edge-on or vanish. With VR the effects use the real head instead: between the
eyes, turned as the headset is. One pose for both eyes: the polygons are built once a frame, and
the eyes' 6 cm apart barely changes the angle. (Refentity sprites and beams are turned per eye
by the renderer.) Cutscene cameras are drawn flat, so they keep the flat view.

==============================================================================
*/

static qboolean	cg_vrFxView;			// this frame's effects face the head
static vec3_t	cg_vrFxOrg;
static vec3_t	cg_vrFxAxis[3];

void CG_VR_SetFXView( qboolean firstPerson ) {
	vrGamePose_t p;

	cg_vrFxView = qfalse;
	if ( !firstPerson || !cg_vrPose.active || !cg_vrPose.head.valid ) {
		return;
	}
	p = cg_vrPose;
	p.hand[0] = p.head;	// VR_HandToWorld places entry 0
	p.hand[0].valid = 1;
	if ( VR_HandToWorld( &p, 0, cg.refdef.vieworg, cg.refdefViewAngles[YAW], cg_vrFxOrg, cg_vrFxAxis ) &&
		VectorLengthSquared( cg_vrFxAxis[0] ) > 0.5f ) {
		cg_vrFxView = qtrue;
	}
}

const float *CG_FXViewOrg( void ) {
	return cg_vrFxView ? cg_vrFxOrg : cg.refdef.vieworg;
}

const vec3_t *CG_FXViewAxis( void ) {
	return cg_vrFxView ? cg_vrFxAxis : cg.refdef.viewaxis;
}

float CG_FXNearCullSq( void ) {
	// in VR the gun is in the hand, about 0.3 to 0.6 m (10 to 20 units) from the eyes: its
	// effects must not be skipped as "too close"
	return cg_vrFxView ? 6.0f * 6.0f : 16.0f * 16.0f;
}

// a sprite drawn by the renderer itself, so it faces each eye
static void CG_VR_Sprite( const vec3_t org, float radius, const vec3_t rgb, float alpha, qhandle_t shader ) {
	refEntity_t ent;

	memset( &ent, 0, sizeof( ent ) );
	ent.reType = RT_SPRITE;
	VectorCopy( org, ent.origin );
	VectorCopy( org, ent.oldorigin );
	ent.radius = radius;
	ent.customShader = shader;
	ent.shaderRGBA[0] = (byte)( rgb[0] * 255 );
	ent.shaderRGBA[1] = (byte)( rgb[1] * 255 );
	ent.shaderRGBA[2] = (byte)( rgb[2] * 255 );
	ent.shaderRGBA[3] = (byte)( alpha * 255 );
	ent.renderfx = RF_FIRST_PERSON;
	cgi_R_AddRefEntityToScene( &ent );
}

// a flat beam turned edge-on to the real head
static void CG_VR_Beam( const vec3_t start, const vec3_t end, float width, const vec3_t rgb, float alpha, qhandle_t shader ) {
	polyVert_t verts[4];
	vec3_t head, dir, toHead, side, mid;
	int i;

	CG_VR_HeadWorld( head );
	VectorSubtract( end, start, dir );
	VectorAdd( start, end, mid );
	VectorScale( mid, 0.5f, mid );
	VectorSubtract( head, mid, toHead );
	CrossProduct( dir, toHead, side );
	if ( VectorNormalize( side ) < 0.0001f ) {
		return;
	}
	VectorScale( side, width * 0.5f, side );
	VectorAdd( start, side, verts[0].xyz );
	VectorSubtract( start, side, verts[1].xyz );
	VectorSubtract( end, side, verts[2].xyz );
	VectorAdd( end, side, verts[3].xyz );
	verts[0].st[0] = 0; verts[0].st[1] = 0;
	verts[1].st[0] = 0; verts[1].st[1] = 1;
	verts[2].st[0] = 1; verts[2].st[1] = 1;
	verts[3].st[0] = 1; verts[3].st[1] = 0;
	for ( i = 0; i < 4; i++ ) {
		verts[i].modulate[0] = (byte)( rgb[0] * 255 );
		verts[i].modulate[1] = (byte)( rgb[1] * 255 );
		verts[i].modulate[2] = (byte)( rgb[2] * 255 );
		verts[i].modulate[3] = (byte)( alpha * 255 );
	}
	cgi_R_AddPolyToScene( shader, 4, verts );
}

static void CG_VR_OffHandMarker( void ) {
	vec3_t org, axis[3], rgb = { 0.55f, 0.75f, 1.0f };

	if ( !cg_vrOffHand.integer ||
		!VR_HandToWorld( &cg_vrPose, VR_HAND_OFFHAND, cg.refdef.vieworg, cg.refdefViewAngles[YAW], org, axis ) ) {
		return;
	}
	CG_VR_Sprite( org, 0.6f, rgb, 0.7f, cgs.media.yellowParticleShader );
}

/*
==============================================================================

THE USE HIGHLIGHT

The box the game draws round a thing the player can use or look up is projected on the screen
through the body's flat view, which in VR is neither where the head looks nor where the HUD
plane is. In VR the corners are drawn in the world instead, round the thing, and its text sits
at a fixed place on the HUD (CG_LabelViewEntity).

==============================================================================
*/

// the head's gaze: from between the eyes, along where the headset points
qboolean CG_VR_HeadRay( vec3_t start, vec3_t forward ) {
	vrGamePose_t p;
	vec3_t axis[3];

	if ( !cg_vrPose.active || !cg_vrPose.head.valid ) {
		return qfalse;
	}
	p = cg_vrPose;
	p.hand[0] = p.head;	// VR_HandToWorld places entry 0
	p.hand[0].valid = 1;
	if ( !VR_HandToWorld( &p, 0, cg.refdef.vieworg, cg.refdefViewAngles[YAW], start, axis ) ) {
		return qfalse;
	}
	VectorCopy( axis[0], forward );
	return qtrue;
}

// the weapon hand's pointing ray, with or without a weapon in it
qboolean CG_VR_PointRay( vec3_t start, vec3_t forward ) {
	vec3_t axis[3];

	if ( CG_VR_AimRay( start, forward ) ) {
		return qtrue;
	}
	if ( !cg_vrPose.active || !VR_HandToWorld( &cg_vrPose, VR_HAND_WEAPON, cg.refdef.vieworg,
			cg.refdefViewAngles[YAW], start, axis ) ) {
		return qfalse;
	}
	VectorCopy( axis[0], forward );
	return qtrue;
}

// The label code runs after the 3D scene is built (2D pass), so it only notes what to box; the
// corners are added to the next frame's scene (CG_VR_AddUseBox).
static int		cg_vrBoxEnt = -1;
static int		cg_vrBoxTime;
static vec4_t	cg_vrBoxColor;

static int		cg_vrUsePickTime = -1;	// cg.time of the frame whose box is Use's own target
static vrUsePick_t	cg_vrUsePick;
static qboolean	cg_vrBoxOwnBounds;		// Use's target: box cg_vrBoxMins/Maxs, not the entity's own bounds
static vec3_t	cg_vrBoxMins, cg_vrBoxMaxs;

// the label's colour for a thing (CG_DrawCrosshairNames): its own, or amber
static void CG_VR_ThingColor( const gentity_t *ent, vec4_t color ) {
	VectorCopy( ent->startRGBA, color );
	if ( !color[0] && !color[1] && !color[2] ) {
		color[0] = 0.9f;
		color[1] = 0.7f;
		color[2] = 0.0f;
	}
	color[3] = 1.0f;
}

static void CG_VR_EntBounds( const gentity_t *ent, vec3_t mn, vec3_t mx ) {
	if ( ent->s.solid == SOLID_BMODEL ) {
		VectorCopy( ent->absmin, mn );
		VectorCopy( ent->absmax, mx );
	} else {
		VectorAdd( ent->currentOrigin, ent->mins, mn );
		VectorAdd( ent->currentOrigin, ent->maxs, mx );
	}
}

/*
A switch area (a use trigger) is an invisible box in front of the switch; boxing it outlines empty
space round the player. The thing to box is the switch or console itself: the smallest drawn entity
(not a person, not another trigger) that the area touches or nearly touches, and smaller than the
area (so the lift or door the switch works isn't taken). Without one (the console is part of the
level's geometry) it is the wall the area faces: a small box where a line from its middle, along
the way it wants to be faced, meets the world. Display only: Use still acts on the trigger.
*/
#define VR_USE_SHOW_MAX	64
static gentity_t *CG_VR_TriggerShow( const gentity_t *trig, vec3_t mn, vec3_t mx ) {
	gentity_t *list[VR_USE_SHOW_MAX], *best = NULL;
	vec3_t tmn, tmx, emn, emx, size, mid, end;
	float tvol, vol, bestVol = 0.0f;
	trace_t tr;
	int i, n, axis;

	VectorCopy( trig->absmin, tmn );
	VectorCopy( trig->absmax, tmx );
	VectorSubtract( tmx, tmn, size );
	tvol = size[0] * size[1] * size[2];
	for ( axis = 0; axis < 3; axis++ ) {
		emn[axis] = tmn[axis] - 8.0f;
		emx[axis] = tmx[axis] + 8.0f;
	}
	n = gi.EntitiesInBox( emn, emx, list, VR_USE_SHOW_MAX );
	for ( i = 0; i < n; i++ ) {
		gentity_t *e = list[i];

		if ( e == trig || !e->inuse || e->client || e->s.number == 0 || ( e->svFlags & SVF_NOCLIENT )
			|| ( e->s.eFlags & EF_NODRAW ) || !e->s.modelindex
			|| ( ( e->contents & CONTENTS_TRIGGER ) && !( e->contents & CONTENTS_SOLID ) ) ) {	// a mover's contents are all bits
			continue;
		}
		CG_VR_EntBounds( e, emn, emx );
		VectorSubtract( emx, emn, size );
		vol = size[0] * size[1] * size[2];
		if ( vol >= tvol ) {
			continue;
		}
		if ( !best || vol < bestVol ) {
			best = e;
			bestVol = vol;
		}
	}
	if ( best ) {
		CG_VR_EntBounds( best, mn, mx );
		return best;
	}

	VectorAdd( tmn, tmx, mid );
	VectorScale( mid, 0.5f, mid );
	if ( VectorLengthSquared( trig->movedir ) > 0.01f ) {
		VectorMA( mid, 64.0f + 0.5f * VectorLength( size ), trig->movedir, end );
		gi.trace( &tr, mid, NULL, NULL, end, ENTITYNUM_NONE, MASK_SOLID );
		if ( tr.fraction < 1.0f && !tr.startsolid ) {
			VectorMA( tr.endpos, 2.0f, tr.plane.normal, mid );
			for ( axis = 0; axis < 3; axis++ ) {
				mn[axis] = mid[axis] - 6.0f;
				mx[axis] = mid[axis] + 6.0f;
			}
			return NULL;
		}
	}
	VectorCopy( tmn, mn );
	VectorCopy( tmx, mx );
	return NULL;
}

void CG_VR_NoteUseBox( const gentity_t *ent, const vec4_t color ) {
	if ( cg_vrUsePickTime == cg.time && cg_vrUsePick.kind != VR_USE_NONE ) {
		return;	// Use's own target is boxed this frame (CG_VR_UsePick)
	}
	cg_vrBoxEnt = ent->s.number;
	cg_vrBoxTime = cg.time;
	cg_vrBoxOwnBounds = qfalse;
	Vector4Copy( color, cg_vrBoxColor );
}

/*
What Use would act on now (G_VRUsePick, the same call the game's Use makes), boxed like a scanned
thing's label (its colour, amber by default); for a switch area, round the switch it belongs to
(CG_VR_TriggerShow). The eye, yaw and pose of this frame are handed to the game's Use (vr_useView),
so the press uses what is boxed. Player 1 only (host or single player): a guest's Use is the host's.
*/
extern bool in_camera;

static void CG_VR_UsePick( void ) {
	gentity_t *player = &g_entities[0];

	cg_vrUsePickTime = -1;
	if ( !cg_vrPose.active || !cg.snap || cg.snap->ps.clientNum != 0 || COOP_IsReplica() || cg.renderingThirdPerson || in_camera
		|| !player->inuse || !player->client || cg.snap->ps.stats[STAT_HEALTH] <= 0 ) {
		return;
	}
	vr_useView.time = cg.time ? cg.time : 1;
	VectorCopy( cg.refdef.vieworg, vr_useView.eye );
	vr_useView.yaw = cg.refdefViewAngles[YAW];
	vr_useView.pose = cg_vrPose;
	if ( !G_VRUsePick( player, &cg_vrPose, vr_useView.eye, vr_useView.yaw, &cg_vrUsePick ) ) {
		return;
	}
	cg_vrUsePickTime = cg.time;
	if ( cg_vrUsePick.kind != VR_USE_NONE && cg_vrUsePick.entityNum < ENTITYNUM_WORLD ) {
		const gentity_t *picked = &g_entities[cg_vrUsePick.entityNum];
		const gentity_t *shown = picked;

		cg_vrBoxEnt = cg_vrUsePick.entityNum;
		cg_vrBoxTime = cg.time;
		cg_vrBoxOwnBounds = qfalse;
		if ( cg_vrUsePick.kind == VR_USE_TRIGGER ) {
			shown = CG_VR_TriggerShow( picked, cg_vrBoxMins, cg_vrBoxMaxs );
			cg_vrBoxOwnBounds = qtrue;
			if ( !shown ) {
				shown = picked;
			}
		}
		CG_VR_ThingColor( shown, cg_vrBoxColor );
		if ( cg_vrGunInfo.integer == 2 ) {
			static int next;
			if ( cg.time >= next || cg.time < next - 2000 ) {
				next = cg.time + 1000;
				CG_Printf( "VR use box: pick %d %s kind %d via %d, shown %d %s, box %.0f %.0f %.0f .. %.0f %.0f %.0f\n",
					picked->s.number, picked->classname, cg_vrUsePick.kind, cg_vrUsePick.via, shown->s.number,
					shown->classname, cg_vrBoxOwnBounds ? cg_vrBoxMins[0] : picked->absmin[0],
					cg_vrBoxOwnBounds ? cg_vrBoxMins[1] : picked->absmin[1], cg_vrBoxOwnBounds ? cg_vrBoxMins[2] : picked->absmin[2],
					cg_vrBoxOwnBounds ? cg_vrBoxMaxs[0] : picked->absmax[0], cg_vrBoxOwnBounds ? cg_vrBoxMaxs[1] : picked->absmax[1],
					cg_vrBoxOwnBounds ? cg_vrBoxMaxs[2] : picked->absmax[2] );
			}
		}
	}
}

// corner brackets round the thing's box, in the world, turned to the head
void CG_VR_AddUseBox( void ) {
	CG_VR_UsePick();

	const gentity_t *ent;
	const float *color = cg_vrBoxColor;
	vec3_t mn, mx, size, head, rgb, a, b;
	float dist, width, len[3];
	int corner, axis;

	if ( !cg_vrPose.active || cg_vrBoxEnt < 0 || cg.time - cg_vrBoxTime > 200 || cg.time < cg_vrBoxTime ) {
		return;
	}
	ent = &g_entities[cg_vrBoxEnt];
	if ( !ent->inuse ) {
		return;
	}

	if ( cg_vrBoxOwnBounds && cg_vrUsePickTime == cg_vrBoxTime ) {
		VectorCopy( cg_vrBoxMins, mn );
		VectorCopy( cg_vrBoxMaxs, mx );
	} else {
		CG_VR_EntBounds( ent, mn, mx );
	}
	for ( axis = 0; axis < 3; axis++ ) {
		mn[axis] -= 1.0f;	// just outside, so a console set into a wall isn't hidden by it
		mx[axis] += 1.0f;
	}
	VectorSubtract( mx, mn, size );
	CG_VR_HeadWorld( head );
	VectorAdd( mn, mx, a );
	VectorScale( a, 0.5f, a );
	dist = Distance( head, a );
	width = 0.5f + dist * 0.008f;	// about the same size to the eye at any distance
	for ( axis = 0; axis < 3; axis++ ) {
		len[axis] = size[axis] * 0.3f;
		if ( len[axis] > 8.0f + dist * 0.02f ) {
			len[axis] = 8.0f + dist * 0.02f;
		}
		if ( len[axis] < 1.5f ) {
			len[axis] = 1.5f;
		}
	}
	rgb[0] = color[0];
	rgb[1] = color[1];
	rgb[2] = color[2];
	for ( corner = 0; corner < 8; corner++ ) {
		for ( axis = 0; axis < 3; axis++ ) {
			a[axis] = ( corner & ( 1 << axis ) ) ? mx[axis] : mn[axis];
		}
		for ( axis = 0; axis < 3; axis++ ) {
			VectorCopy( a, b );
			b[axis] += ( corner & ( 1 << axis ) ) ? -len[axis] : len[axis];
			CG_VR_Beam( a, b, width, rgb, 0.9f, cgs.media.whiteLaserShader );
		}
	}
}

static int		cg_vrAimRayTime = -1;
static vec3_t	cg_vrAimStart, cg_vrAimForward;

qboolean CG_VR_AimRay( vec3_t start, vec3_t forward ) {
	if ( !cg_vrPose.active || cg_vrAimRayTime != cg.time ) {
		return qfalse;
	}
	VectorCopy( cg_vrAimStart, start );
	VectorCopy( cg_vrAimForward, forward );
	return qtrue;
}

// the newest shot ray, if drawn at most maxAge ms ago (a predicted shot runs before this frame's
// view weapon is placed)
qboolean CG_VR_RecentAimRay( vec3_t start, vec3_t forward, int maxAge ) {
	if ( !cg_vrPose.active || !cg_vrAimRayTime || cg.time < cg_vrAimRayTime || cg.time - cg_vrAimRayTime > maxAge ) {
		return qfalse;
	}
	VectorCopy( cg_vrAimStart, start );
	VectorCopy( cg_vrAimForward, forward );
	return qtrue;
}

// co-op guest: the engine sends the weapon hand with the next commands (CG_VR_SETAIM): the muzzle
// as drawn, from the eye, and the hand's ray, both in the body frame before the yaw
static void CG_VR_ReportAim( const vec3_t muzzle, const vec3_t forward ) {
	const float yaw = DEG2RAD( cg.refdefViewAngles[YAW] ), c = cos( yaw ), s = sin( yaw );
	vec3_t d;
	float aim[6];

	VectorSubtract( muzzle, cg.refdef.vieworg, d );
	aim[0] = c * d[0] + s * d[1];
	aim[1] = -s * d[0] + c * d[1];
	aim[2] = d[2];
	aim[3] = c * forward[0] + s * forward[1];
	aim[4] = -s * forward[0] + c * forward[1];
	aim[5] = forward[2];
	syscall( CG_VR_SETAIM, aim );
}

void CG_VR_AddAimHelpers( const vec3_t muzzle ) {
	trace_t tr;
	vec3_t start, forward, end, rgb = { 1.0f, 0.15f, 0.1f };
	float dist;

	CG_VR_OffHandMarker();
	if ( !CG_VR_ShotRay( muzzle, start, forward ) ) {
		return;
	}
	VectorCopy( start, cg_vrAimStart );
	VectorCopy( forward, cg_vrAimForward );
	cg_vrAimRayTime = cg.time;
	CG_VR_ReportAim( muzzle, forward );
	if ( !cg_vrLaser.integer || ( cg.snap->ps.weapon >= WP_TRICORDER && cg.snap->ps.weapon <= WP_RED_HYPO ) ) {
		// no laser on the tricorder and the hypos
		return;
	}
	VectorMA( start, 8192, forward, end );
	CG_Trace( &tr, start, NULL, NULL, end, cg.snap->ps.clientNum, MASK_SHOT );
	if ( tr.startsolid ) {
		return;
	}
	dist = Distance( start, tr.endpos );
	if ( cg_vrGunInfo.integer == 2 ) {
		static int next;
		if ( cg.time >= next || cg.time < next - 2000 ) {
			next = cg.time + 1000;
			CG_Printf( "VR aim: time %d eye %.1f %.1f %.1f muzzle %.1f %.1f %.1f start %.1f %.1f %.1f "
				"fwd %.2f %.2f %.2f end %.1f %.1f %.1f frac %.3f solid %d\n", cg.time,
				cg.refdef.vieworg[0], cg.refdef.vieworg[1], cg.refdef.vieworg[2], muzzle[0], muzzle[1], muzzle[2],
				start[0], start[1], start[2], forward[0], forward[1], forward[2],
				tr.endpos[0], tr.endpos[1], tr.endpos[2], tr.fraction, tr.startsolid );
		}
	}
	if ( cg_vrLaser.integer == 1 ) {
		CG_VR_Beam( start, tr.endpos, 0.3f, rgb, 0.6f, cgs.media.whiteLaserShader );
	}
	if ( tr.fraction < 1.0f ) {
		// the dot: about the same size to the eye at any distance
		vec3_t dot;

		VectorMA( tr.endpos, 0.5f, tr.plane.normal, dot );
		CG_VR_Sprite( dot, 0.5f + dist * 0.003f, rgb, 1.0f, cgs.media.yellowParticleShader );
	}
}
