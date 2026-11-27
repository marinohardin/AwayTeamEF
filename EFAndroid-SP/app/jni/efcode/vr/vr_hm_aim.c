/*
===========================================================================
AwayTeamEF VR: Holomatch H1, the aim from the hand (EF_VR only).

This file is part of AwayTeamEF and is licensed under the GNU General
Public License, version 2 or (at your option) any later version.
===========================================================================

Compiled into the engine target (an INTERFACE source of ef_vr). docs/vr/HOLOMATCH-H1.md §3.

A Holomatch server fires from its own muzzle beside the eye, turned towards whatever the eye
ray along the view angles hits first. So the engine sends the view angles that make the
server's eye ray go through P, the first thing along the hand's ray: the shot then lands where
the hand points. It is parallax correction, not target selection.

Each command (VR_HM_Cmd, from CL_CreateCmd after CL_FinishMove):
- the body yaw is cl.viewangles[YAW] (turning, the mouse and vr_debugTurn add to it, as
  before) plus the snapshot's delta angles: the world body yaw. The renderer gets it
  (VR_SetBodyYaw), so the world no longer turns with the gun;
- the weapon hand (filtered, about like a 1-euro filter: still at rest, no lag when fast) is
  put in the world with the shared convention, world = eye + R(body yaw) * offset;
- the eye-to-muzzle check: a gun pushed into a wall shoots that wall;
- P = the first hit of the hand's ray (vr_hm_trace.c), up to 8192 units;
- the world aim = the angles from the aim eye E to P, pitch clamped to +-87 degrees (inside
  the game's own clamp, so it never rewrites the delta angles), roll 0;
- the command's angles = the world aim minus the newest snapshot's delta angles, so a
  teleport or a respawn (which change the delta) turns the body as for any player;
- the command's movement is turned from the body's frame into the aim's, because the game
  moves the player relative to the command's angles.
Before each world scene (VR_HM_Scene): the cgame's view is recorded (the next aim eye) and the
laser and the dot are drawn. The dot repeats the server's maths with the angles really sent:
the eye ray to Q, the muzzle from the game's table, the eye-to-muzzle check, and the shot from
the muzzle to Q. When Q isn't P the dot shows a "blocked" colour: it never promises a hit the
server won't give.
*/

#include "../client/client.h"
#include "vr.h"
#include "vr_input.h"
#include "vr_hm.h"

#define VR_HM_RANGE			8192.0f
#define VR_HM_PITCH_LIMIT	87.0f		// the game clamps view pitch at 16000 short units (87.9)
#define VR_HM_MASK_SHOT		( CONTENTS_SOLID | CONTENTS_BODY | CONTENTS_CORPSE | CONTENTS_SHOTCLIP )

// The Holomatch weapon numbers used below
#define HM_WP_GRENADE_LAUNCHER	6
#define HM_WP_VOYAGER_HYPO		10
#define HM_WP_BORG_ASSIMILATOR	11
#define HM_WP_NUM_WEAPONS		13

// Per-weapon muzzle offsets (forward, right, up), in game units from the player's eye: forward
// and right along the view, up on the world's Z. This mirrors the Holomatch game module's own
// table (WP_MuzzlePoint in game/g_weapon.c of cMod by Noah Metzger (Chomenor),
// github.com/Chomenor/cmod-game-code, which keeps the game's values), so the dot can repeat
// where the server's shot starts. Indexed by the player state's weapon number.
static const float vr_hmMuzzle[HM_WP_NUM_WEAPONS][3] = {
	{ 0,	0,		0	},	// none
	{ 29,	2,		-4	},	// phaser
	{ 25,	7,		-10	},	// compression rifle
	{ 25,	4,		-5	},	// I-MOD
	{ 10,	14,		-8	},	// scavenger rifle
	{ 25,	5,		-8	},	// stasis weapon
	{ 25,	5,		-10	},	// grenade launcher
	{ 22,	4.5f,	-8	},	// tetryon disruptor
	{ 5,	6,		-6	},	// quantum burst
	{ 27,	8,		-10	},	// arc welder (dreadnought)
	{ 29,	2,		-4	},	// hypo
	{ 29,	2,		-4	},	// Borg assimilator
	{ 27,	8,		-10	},	// Borg weapon
};

static struct {
	qboolean	registered;
	cvar_t		*laser;
	cvar_t		*filter;
	cvar_t		*minCutoff;
	cvar_t		*beta;
	cvar_t		*muzzleDist;
	cvar_t		*debug;
	cvar_t		*worldScale;

	// the cgame's last world view (the aim eye for the next command)
	qboolean	haveView;
	vec3_t		viewOrg;
	int			viewTime;		// cl.serverTime when it was drawn

	// the filter, in the body frame
	qboolean	filterValid;
	int			filterTime;
	vec3_t		fAxis[2];		// filtered forward and up
	vec3_t		fSpeed[2];		// filtered rates of change

	// the newest command's aim, for drawing
	qboolean	aimValid;
	float		bodyYaw;		// world body yaw, degrees
	vec3_t		handOfs;		// hand origin - aim eye, world axes
	vec3_t		handFwd;		// the hand's ray, world
	vec3_t		rayStart;		// where the laser starts: the hand muzzle, or where the eye-to-muzzle check stopped
	qboolean	muzzleBlocked;
	vec3_t		P;				// first hit along the hand's ray
	vec3_t		Q;				// where the server's shot is predicted to land
	qboolean	qHit;			// the shot hits something within range
	qboolean	qBlocked;		// Q isn't P
	vec3_t		M;				// the server's predicted muzzle (the eye when the eye-to-muzzle check fails)
	int			weapon;
	vec3_t		eye;			// the aim eye used
	short		sent[3];		// the command's angles
	int			lastPrint;
} vha;

static void VR_HM_AimRegister( void ) {
	vha.laser = Cvar_Get( "vr_hmLaser", "1", CVAR_ARCHIVE );
	Cvar_CheckRange( vha.laser, 0, 2, qtrue );
	Cvar_SetDescription( vha.laser, "VR, Holomatch: 1 = a laser from the gun and a dot where the shot will land, 2 = the dot only, 0 = neither." );
	vha.filter = Cvar_Get( "vr_hmAimFilter", "1", CVAR_ARCHIVE );
	Cvar_CheckRange( vha.filter, 0, 1, qtrue );
	Cvar_SetDescription( vha.filter, "VR, Holomatch: smooth the hand's aim (1) or not (0). Still at rest, no lag when moving fast." );
	vha.minCutoff = Cvar_Get( "vr_hmAimFilterCutoff", "1.5", CVAR_ARCHIVE );
	Cvar_CheckRange( vha.minCutoff, 0.1f, 30.0f, qfalse );
	Cvar_SetDescription( vha.minCutoff, "VR, Holomatch: the aim filter's cut-off at rest, Hz (lower = steadier, slower)." );
	vha.beta = Cvar_Get( "vr_hmAimFilterBeta", "5", CVAR_ARCHIVE );
	Cvar_CheckRange( vha.beta, 0.0f, 100.0f, qfalse );
	Cvar_SetDescription( vha.beta, "VR, Holomatch: how fast the aim filter opens up with the hand's turning speed (Hz per radian per second)." );
	vha.muzzleDist = Cvar_Get( "vr_hmMuzzle", "6", CVAR_ARCHIVE );
	Cvar_CheckRange( vha.muzzleDist, 0.0f, 24.0f, qfalse );
	Cvar_SetDescription( vha.muzzleDist, "VR, Holomatch: game units from the controller's aim point to the gun's muzzle, where the laser starts." );
	vha.debug = Cvar_Get( "vr_hmDebug", "0", CVAR_TEMP );
	Cvar_SetDescription( vha.debug, "VR, Holomatch debugging: 1 = print the aim twice a second, 2 = every command, "
		"3 = also the snapshots' event entities (where the server's shots went)." );
	vha.worldScale = Cvar_Get( "vr_worldScale", "32", 0 );
	vha.registered = qtrue;
}

void VR_HM_AimReset( void ) {
	vha.haveView = qfalse;
	vha.filterValid = qfalse;
	vha.aimValid = qfalse;
	VR_SetBodyYaw( 0.0f, 0 );
}

// body frame (forward, left, up) -> world, turned by the body yaw
static void VR_HM_ToWorld( float yawDeg, const vec3_t in, vec3_t out ) {
	const float a = DEG2RAD( yawDeg ), c = cosf( a ), s = sinf( a );
	const float x = in[0], y = in[1];

	out[0] = c * x - s * y;
	out[1] = s * x + c * y;
	out[2] = in[2];
}

// one step of a speed-adaptive low-pass (1-euro style) on a unit vector
static void VR_HM_FilterAxis( vec3_t state, vec3_t speed, const vec3_t in, float dt ) {
	const float dCutoff = 1.0f;
	float aD, rate, cutoff, a, tau;
	vec3_t d;
	int i;

	VectorSubtract( in, state, d );
	VectorScale( d, 1.0f / dt, d );
	tau = 1.0f / ( 2.0f * (float)M_PI * dCutoff );
	aD = 1.0f / ( 1.0f + tau / dt );
	for ( i = 0; i < 3; i++ ) {
		speed[i] += aD * ( d[i] - speed[i] );
	}
	rate = VectorLength( speed );		// about radians per second for a unit vector
	cutoff = vha.minCutoff->value + vha.beta->value * rate;
	tau = 1.0f / ( 2.0f * (float)M_PI * cutoff );
	a = 1.0f / ( 1.0f + tau / dt );
	for ( i = 0; i < 3; i++ ) {
		state[i] += a * ( in[i] - state[i] );
	}
	VectorNormalize( state );
}

static void VR_HM_Filter( vrBodyHand_t *hand ) {
	const int now = Sys_Milliseconds();
	float dt;
	int i;

	if ( !vha.filter->integer || !vha.filterValid ) {
		VectorCopy( hand->axis[0], vha.fAxis[0] );
		VectorCopy( hand->axis[2], vha.fAxis[1] );
		VectorClear( vha.fSpeed[0] );
		VectorClear( vha.fSpeed[1] );
		vha.filterValid = qtrue;
		vha.filterTime = now;
		return;
	}
	dt = ( now - vha.filterTime ) * 0.001f;
	vha.filterTime = now;
	if ( dt <= 0.0f ) {
		dt = 0.001f;
	} else if ( dt > 0.1f ) {
		dt = 0.1f;
	}
	VR_HM_FilterAxis( vha.fAxis[0], vha.fSpeed[0], hand->axis[0], dt );
	VR_HM_FilterAxis( vha.fAxis[1], vha.fSpeed[1], hand->axis[2], dt );

	// back to an orthonormal frame: forward as filtered, up made square to it, left from both
	VectorCopy( vha.fAxis[0], hand->axis[0] );
	{
		const float d = DotProduct( vha.fAxis[1], hand->axis[0] );
		for ( i = 0; i < 3; i++ ) {
			hand->axis[2][i] = vha.fAxis[1][i] - d * hand->axis[0][i];
		}
	}
	VectorNormalize( hand->axis[2] );
	CrossProduct( hand->axis[2], hand->axis[0], hand->axis[1] );
}

// the server's shot for these view angles, from the aim eye: where it lands (Q)
static void VR_HM_PredictShot( const vec3_t eye, const vec3_t viewAngles, int weapon, int self ) {
	vec3_t fwd, right, up, end, muzzle, dir;
	vrHmTrace_t tr;

	AngleVectors( viewAngles, fwd, right, up );
	VectorMA( eye, VR_HM_RANGE, fwd, end );
	VR_HM_Trace( &tr, eye, end, NULL, NULL, self, VR_HM_MASK_SHOT );	// the eye ray: Q0
	VectorCopy( tr.endpos, vha.Q );
	vha.qHit = tr.fraction < 1.0f ? qtrue : qfalse;

	VectorCopy( eye, vha.M );
	if ( weapon <= 0 || weapon >= HM_WP_NUM_WEAPONS ) {
		return;
	}
	VectorCopy( eye, muzzle );
	VectorMA( muzzle, vr_hmMuzzle[weapon][0], fwd, muzzle );
	VectorMA( muzzle, vr_hmMuzzle[weapon][1], right, muzzle );
	muzzle[2] += vr_hmMuzzle[weapon][2];

	VR_HM_Trace( &tr, eye, muzzle, NULL, NULL, self, VR_HM_MASK_SHOT );
	if ( tr.fraction < 1.0f ) {
		return;		// the server fires from the eye, straight along the view: Q0
	}
	VectorCopy( muzzle, vha.M );
	VectorSubtract( vha.Q, muzzle, dir );
	if ( VectorNormalize( dir ) < 0.001f ) {
		return;
	}
	VectorMA( muzzle, VR_HM_RANGE, dir, end );
	VR_HM_Trace( &tr, muzzle, end, NULL, NULL, self, VR_HM_MASK_SHOT );
	if ( Distance( tr.endpos, muzzle ) < Distance( vha.Q, muzzle ) - 1.0f ) {
		VectorCopy( tr.endpos, vha.Q );	// something between the muzzle and Q0
		vha.qHit = tr.fraction < 1.0f ? qtrue : qfalse;
	}
}

static void VR_HM_DebugEvents( void ) {
	static int nextPlayers;
	const qboolean players = Sys_Milliseconds() >= nextPlayers ? qtrue : qfalse;
	int i;

	if ( players ) {
		nextPlayers = Sys_Milliseconds() + 1000;
	}
	for ( i = 0; i < cl.snap.numEntities; i++ ) {
		const entityState_t *es = &cl.parseEntities[( cl.snap.parseEntitiesNum + i ) & ( MAX_PARSE_ENTITIES - 1 )];
		static int lastNum[MAX_GENTITIES], lastTime[MAX_GENTITIES];

		if ( players && es->solid && es->solid != SOLID_BMODEL ) {
			Com_Printf( "vrhm solid: snap %d ent %d etype %d base %.1f %.1f %.1f solid 0x%x\n", cl.snap.serverTime,
				es->number, es->eType, es->pos.trBase[0], es->pos.trBase[1], es->pos.trBase[2], es->solid );
		}

		// the game's freestanding events (eType 13 and up in Holomatch): print each once
		if ( es->eType < 13 || ( lastNum[es->number] == es->eType && lastTime[es->number] == es->pos.trTime ) ) {
			continue;
		}
		lastNum[es->number] = es->eType;
		lastTime[es->number] = es->pos.trTime;
		Com_Printf( "vrhm event: snap %d ent %d etype %d base %.1f %.1f %.1f origin2 %.1f %.1f %.1f other %d\n",
			cl.snap.serverTime, es->number, es->eType, es->pos.trBase[0], es->pos.trBase[1], es->pos.trBase[2],
			es->origin2[0], es->origin2[1], es->origin2[2], es->otherEntityNum );
	}
}

void VR_HM_Cmd( usercmd_t *cmd ) {
	const playerState_t *ps = &cl.snap.ps;
	const int active = VR_HM_Active();
	const int self = ps->clientNum;
	vrHmState_t state;
	vrBodyHand_t hand, head;
	vec3_t eye, worldAim, dir, headOfs, renderEye, viewAngles;
	float bodyYaw, dt, aimYawDelta;
	int i;

	if ( !vha.registered ) {
		VR_HM_AimRegister();
	}
	VR_HM_UpdateCvars( active );
	if ( !active ) {
		return;
	}
	if ( vha.debug->integer >= 3 ) {
		VR_HM_DebugEvents();
	}

	bodyYaw = AngleNormalize360( cl.viewangles[YAW] + SHORT2ANGLE( ps->delta_angles[YAW] ) );
	vha.bodyYaw = bodyYaw;
	VR_SetBodyYaw( bodyYaw, 1 );

	state = VR_HM_State();
	vha.aimValid = qfalse;
	if ( !vha.haveView || state == VR_HM_FOLLOW || state == VR_HM_INTERMISSION ) {
		return;		// the server doesn't take the angles (or there is no view yet): as before
	}

	// the aim eye: the cgame's last view, moved on by the velocity to this command's time
	dt = ( cmd->serverTime - vha.viewTime ) * 0.001f;
	dt = dt < 0.0f ? 0.0f : dt > 0.1f ? 0.1f : dt;
	VectorMA( vha.viewOrg, dt, ps->velocity, eye );
	VectorCopy( eye, vha.eye );

	VectorClear( headOfs );
	if ( VR_EngineBodyHand( VR_BODY_HEAD, &head ) ) {
		VR_HM_ToWorld( bodyYaw, head.origin, headOfs );
	}
	VectorAdd( eye, headOfs, renderEye );

	if ( state == VR_HM_SPECTATOR || !VR_EngineBodyHand( VR_BODY_WEAPON, &hand ) ) {
		// a free spectator flies where the head looks; no tracked hand: aim with the head
		if ( !head.valid ) {
			return;
		}
		VR_HM_ToWorld( bodyYaw, head.axis[0], dir );
		vectoangles( dir, worldAim );
	} else {
		vec3_t muzzle, end;
		vrHmTrace_t tr;

		VR_HM_Filter( &hand );
		VR_HM_ToWorld( bodyYaw, hand.origin, vha.handOfs );
		VR_HM_ToWorld( bodyYaw, hand.axis[0], vha.handFwd );
		VectorAdd( eye, vha.handOfs, muzzle );
		VectorMA( muzzle, vha.muzzleDist->value, vha.handFwd, muzzle );

		// a gun pushed into a wall shoots that wall
		VR_HM_Trace( &tr, renderEye, muzzle, NULL, NULL, self, VR_HM_MASK_SHOT );
		vha.muzzleBlocked = tr.fraction < 1.0f ? qtrue : qfalse;
		VectorCopy( tr.endpos, vha.rayStart );
		if ( vha.muzzleBlocked ) {
			VectorCopy( tr.endpos, vha.P );
		} else {
			VectorMA( vha.rayStart, VR_HM_RANGE, vha.handFwd, end );
			VR_HM_Trace( &tr, vha.rayStart, end, NULL, NULL, self, VR_HM_MASK_SHOT );
			VectorCopy( tr.endpos, vha.P );
		}
		VectorSubtract( vha.P, eye, dir );
		if ( VectorLength( dir ) < 1.0f ) {
			VectorCopy( vha.handFwd, dir );
		}
		vectoangles( dir, worldAim );
		vha.aimValid = qtrue;
	}

	// the pitch stays inside the game's clamp, so the server never rewrites the delta angles
	worldAim[PITCH] = AngleNormalize180( worldAim[PITCH] );
	if ( worldAim[PITCH] > VR_HM_PITCH_LIMIT ) {
		worldAim[PITCH] = VR_HM_PITCH_LIMIT;
	} else if ( worldAim[PITCH] < -VR_HM_PITCH_LIMIT ) {
		worldAim[PITCH] = -VR_HM_PITCH_LIMIT;
	}
	worldAim[ROLL] = 0.0f;

	for ( i = 0; i < 3; i++ ) {
		cmd->angles[i] = ANGLE2SHORT( worldAim[i] - SHORT2ANGLE( ps->delta_angles[i] ) );
		vha.sent[i] = (short)cmd->angles[i];
		// what the server will make of it (PM_UpdateViewAngles: command plus delta, as shorts)
		viewAngles[i] = SHORT2ANGLE( (short)( cmd->angles[i] + ps->delta_angles[i] ) );
	}

	// the game moves the player relative to the command's yaw: turn the movement from the
	// body's frame (the stick, the keyboard) into the aim's
	aimYawDelta = DEG2RAD( AngleNormalize180( bodyYaw - viewAngles[YAW] ) );
	if ( cmd->forwardmove || cmd->rightmove ) {
		const float c = cosf( aimYawDelta ), s = sinf( aimYawDelta );
		const float f = cmd->forwardmove, l = -cmd->rightmove;
		const float f2 = f * c - l * s, l2 = f * s + l * c;

		cmd->forwardmove = ClampChar( (int)floorf( f2 + 0.5f ) );
		cmd->rightmove = ClampChar( (int)floorf( -l2 + 0.5f ) );
	}

	vha.weapon = ps->weapon;
	if ( vha.aimValid ) {
		VR_HM_PredictShot( eye, viewAngles, ps->weapon, self );
		vha.qBlocked = Distance( vha.Q, vha.P ) > 2.0f ? qtrue : qfalse;
	}

	if ( vha.debug->integer && ( vha.debug->integer >= 2 || Sys_Milliseconds() - vha.lastPrint >= 500 ) ) {
		vha.lastPrint = Sys_Milliseconds();
		Com_Printf( "vrhm aim: st %d state %d w %d body %.2f eye %.2f %.2f %.2f view %.3f %.3f sent %d %d %d delta %d %d %d "
			"P %.2f %.2f %.2f Q %.2f %.2f %.2f M %.2f %.2f %.2f qhit %d qblock %d mblock %d move %d %d\n",
			cmd->serverTime, state, ps->weapon, bodyYaw, eye[0], eye[1], eye[2], viewAngles[PITCH], viewAngles[YAW],
			vha.sent[0], vha.sent[1], vha.sent[2], ps->delta_angles[0], ps->delta_angles[1], ps->delta_angles[2],
			vha.P[0], vha.P[1], vha.P[2], vha.Q[0], vha.Q[1], vha.Q[2], vha.M[0], vha.M[1], vha.M[2], vha.qHit, vha.qBlocked, vha.muzzleBlocked,
			cmd->forwardmove, cmd->rightmove );
	}
}

// a flat quad from a to b, turned edge-on to the head, with vertex colours (additive shader)
static void VR_HM_Beam( qhandle_t shader, const vec3_t a, const vec3_t b, const vec3_t head, float width, const byte rgba[4] ) {
	polyVert_t v[4];
	vec3_t d, mid, toHead, side;
	int i;

	VectorSubtract( b, a, d );
	VectorAdd( a, b, mid );
	VectorScale( mid, 0.5f, mid );
	VectorSubtract( head, mid, toHead );
	CrossProduct( d, toHead, side );
	if ( VectorNormalize( side ) < 0.0001f ) {
		return;
	}
	VectorScale( side, width * 0.5f, side );
	VectorAdd( a, side, v[0].xyz );
	VectorSubtract( a, side, v[1].xyz );
	VectorSubtract( b, side, v[2].xyz );
	VectorAdd( b, side, v[3].xyz );
	v[0].st[0] = 0; v[0].st[1] = 0;
	v[1].st[0] = 0; v[1].st[1] = 1;
	v[2].st[0] = 1; v[2].st[1] = 1;
	v[3].st[0] = 1; v[3].st[1] = 0;
	for ( i = 0; i < 4; i++ ) {
		Com_Memcpy( v[i].modulate, rgba, 4 );
	}
	re.AddPolyToScene( shader, 4, v, 1 );
}

// a square facing the head
static void VR_HM_Dot( qhandle_t shader, const vec3_t at, const vec3_t head, float size, const byte rgba[4] ) {
	polyVert_t v[4];
	vec3_t toHead, right, up, z = { 0, 0, 1 };
	int i;

	VectorSubtract( head, at, toHead );
	if ( VectorNormalize( toHead ) < 0.001f ) {
		return;
	}
	CrossProduct( toHead, z, right );
	if ( VectorNormalize( right ) < 0.001f ) {
		VectorSet( right, 0, 1, 0 );
	}
	CrossProduct( right, toHead, up );
	VectorScale( right, size, right );
	VectorScale( up, size, up );
	for ( i = 0; i < 3; i++ ) {
		v[0].xyz[i] = at[i] - right[i] - up[i];
		v[1].xyz[i] = at[i] - right[i] + up[i];
		v[2].xyz[i] = at[i] + right[i] + up[i];
		v[3].xyz[i] = at[i] + right[i] - up[i];
	}
	v[0].st[0] = 0; v[0].st[1] = 0;
	v[1].st[0] = 0; v[1].st[1] = 1;
	v[2].st[0] = 1; v[2].st[1] = 1;
	v[3].st[0] = 1; v[3].st[1] = 0;
	for ( i = 0; i < 4; i++ ) {
		Com_Memcpy( v[i].modulate, rgba, 4 );
	}
	re.AddPolyToScene( shader, 4, v, 1 );
}

void VR_HM_Scene( const void *refdefPtr ) {
	const refdef_t *fd = (const refdef_t *)refdefPtr;
	static const byte red[4] = { 255, 40, 25, 255 }, redDim[4] = { 120, 20, 12, 255 };
	static const byte blocked[4] = { 255, 170, 0, 255 }, laserRgb[4] = { 150, 25, 15, 255 };
	vec3_t start, head, headOfs;
	vrBodyHand_t h;
	qhandle_t laserShader, dotShader;

	if ( !vha.registered || !fd || ( fd->rdflags & RDF_NOWORLDMODEL ) || !VR_HM_Active() ) {
		return;
	}
	VectorCopy( fd->vieworg, vha.viewOrg );
	vha.viewTime = cl.serverTime;
	vha.haveView = qtrue;

	if ( !vha.aimValid || !vha.laser->integer || VR_HM_State() != VR_HM_PLAY ) {
		return;
	}

	// from this frame's view: the hand moved with the body since the command was made
	VectorClear( headOfs );
	if ( VR_EngineBodyHand( VR_BODY_HEAD, &h ) ) {
		VR_HM_ToWorld( vha.bodyYaw, h.origin, headOfs );
	}
	VectorAdd( fd->vieworg, headOfs, head );
	{
		vec3_t shift;
		VectorSubtract( fd->vieworg, vha.eye, shift );
		VectorAdd( vha.rayStart, shift, start );
	}

	laserShader = re.RegisterShader( "gfx/effects/whitelaser" );
	dotShader = re.RegisterShader( "gfx/misc/yellowparticle" );

	if ( vha.laser->integer == 1 && !vha.muzzleBlocked ) {
		VR_HM_Beam( laserShader, start, vha.P, head, 0.3f, vha.qBlocked ? redDim : laserRgb );
	}
	if ( vha.weapon == HM_WP_GRENADE_LAUNCHER || vha.weapon == HM_WP_VOYAGER_HYPO || vha.weapon == HM_WP_BORG_ASSIMILATOR ) {
		return;		// an arcing shot or a touch weapon: no dot
	}
	if ( vha.qHit ) {
		const float dist = Distance( head, vha.Q );
		VR_HM_Dot( dotShader, vha.Q, head, 0.5f + dist * 0.003f, vha.qBlocked ? blocked : red );
	}
}
