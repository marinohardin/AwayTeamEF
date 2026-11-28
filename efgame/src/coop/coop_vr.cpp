// coop_vr.cpp -- single-player co-op with a VR player on either end.
//
// - A VR guest's commands bring its weapon hand (BUTTON_VRAIM, then the bridge's GuestVRAim): the
//   muzzle its cgame drew, as an offset from its eye, and the hand's pointing ray, both in the body
//   frame before the yaw. Player 2's shots leave from there (W_VRHandShot, g_weapon.cpp), with the
//   same eye-to-muzzle trace as a VR player 1's.
// - Both machines draw a VR player's model with its head and torso turned toward the hand's aim
//   (CG_PlayerAngles): the host knows both players' aims (its own headset, the guest's commands)
//   and sends them in the replica's globals, as an optional chunk that only exists while a VR
//   player has a tracked weapon hand. A flat player's model is drawn exactly as before.
// - The lean needs nothing here: a VR player's head drives leanofs in its own commands
//   (BUTTON_VRLEAN, PM_VRLean), which the replica carries like the key lean.

#include "g_local.h"
#include "coop_public.h"
#include "coop_local.h"
#include "vr_shared.h"

extern qboolean W_VRHandAim( gentity_t *ent, vec3_t start, vec3_t forward );	// g_weapon.cpp

typedef struct
{
	int		num;		// the player's entity number
	short	angles[2];	// pitch and yaw of the weapon hand's ray (ANGLE2SHORT)
} coopVRAim_t;

static coopVRAim_t	cv_rep[2];		// guest: the host's aims, from the newest frame
static int			cv_repCount;
static cvar_t		*cv_debug;

qboolean COOP_VRDebugShots( void )
{
	if ( !cv_debug )
	{
		cv_debug = gi.cvar( "coop_vrDebug", "0", 0 );
	}
	return cv_debug->integer ? qtrue : qfalse;
}

// the body frame (+X forward, +Y left, +Z up) turned by the yaw, into the world
static void CV_BodyToWorld( const vec3_t in, float yaw, vec3_t out )
{
	const float c = cos( DEG2RAD( yaw ) ), s = sin( DEG2RAD( yaw ) );

	out[0] = c * in[0] - s * in[1];
	out[1] = s * in[0] + c * in[1];
	out[2] = in[2];
}

qboolean COOP_VRGuestHand( gentity_t *ent, const vec3_t eye, vec3_t muzzle, vec3_t forward )
{
	vec3_t ofs;

	if ( !coop_enabled || !ent->client || !COOP_IsP2( ent ) || !coop_p2.vrAim )
	{
		return qfalse;
	}
	CV_BodyToWorld( coop_p2.vrMuzzle, ent->client->ps.viewangles[YAW], ofs );
	VectorAdd( eye, ofs, muzzle );
	CV_BodyToWorld( coop_p2.vrForward, ent->client->ps.viewangles[YAW], forward );
	if ( VectorNormalize( forward ) < 0.5f )
	{
		return qfalse;
	}
	return qtrue;
}

// the host: where a player's weapon hand points, in the world; qfalse for a flat player
static qboolean CV_HostAim( const gentity_t *ent, vec3_t angles )
{
	vec3_t forward;

	if ( !ent->client )
	{
		return qfalse;
	}
	if ( ent->s.number == 0 )
	{
		vrGamePose_t	pose;
		vec3_t			org, axis[3];

		if ( !VR_FetchPose( &pose ) ||
			!VR_HandToWorld( &pose, VR_HAND_WEAPON, ent->currentOrigin, ent->client->ps.viewangles[YAW], org, axis ) )
		{
			return qfalse;
		}
		VectorCopy( axis[0], forward );
	}
	else if ( COOP_IsP2( ent ) && coop_p2.vrAim )
	{
		CV_BodyToWorld( coop_p2.vrForward, ent->client->ps.viewangles[YAW], forward );
	}
	else
	{
		return qfalse;
	}
	if ( VectorNormalize( forward ) < 0.5f )
	{
		return qfalse;
	}
	vectoangles( forward, angles );
	angles[ROLL] = 0;
	return qtrue;
}

qboolean COOP_VRAimAngles( const gentity_t *ent, vec3_t angles )
{
	if ( !ent || !COOP_IsPlayer( ent ) )
	{
		return qfalse;
	}
	if ( coop_replica )
	{
		for ( int i = 0; i < cv_repCount; i++ )
		{
			if ( cv_rep[i].num == ent->s.number )
			{
				angles[PITCH] = SHORT2ANGLE( cv_rep[i].angles[0] );
				angles[YAW] = SHORT2ANGLE( cv_rep[i].angles[1] );
				angles[ROLL] = 0;
				return qtrue;
			}
		}
		return qfalse;
	}
	return coop_enabled ? CV_HostAim( ent, angles ) : qfalse;
}

// A VR player's eye for the entity's angles: its origin plus its view height. A partner's cgame
// knows the same point (COOP_VRBeamEye), so it can trace from there along the angles.
static void CV_EntityEye( const vec3_t origin, int viewheight, vec3_t eye )
{
	VectorCopy( origin, eye );
	eye[2] += viewheight;
}

/*
COOP_VRAimEntity

The host, after each command a player's ClientThink applied: a VR player's view angles are only
its body's yaw, but a partner draws its beam weapons (the phaser, the Dreadnought's arc) and its
model from the entity's angles (s.apos). So for a VR player those angles look from its eye at the
spot its weapon hand points at: the end of the hand's own ray (W_VRHandAim, the same muzzle and
ray as its shots), at the held weapon's range. Any partner's cgame, from this version or an older
one, then draws the beam from the model's gun to where the hand really shoots, the way it draws a
flat player's. The player's own view, its shots and its saved angles (ps.viewangles) don't change.
*/
void COOP_VRAimEntity( gentity_t *ent )
{
	vec3_t	start, forward, end, eye, dir, angles;
	trace_t	tr;
	float	range;

	if ( !coop_enabled || coop_replica || !ent->client || ent->NPC || !COOP_IsPlayer( ent ) ||
		!W_VRHandAim( ent, start, forward ) )
	{
		return;
	}
	range = ( ent->s.weapon > WP_NONE && ent->s.weapon < WP_NUM_WEAPONS ) ? weaponData[ent->s.weapon].range : 0;
	if ( range < 64 )
	{
		range = 8192;
	}
	VectorMA( start, range, forward, end );
	gi.trace( &tr, start, NULL, NULL, end, ent->s.number, MASK_SHOT );
	CV_EntityEye( ent->client->ps.origin, ent->client->ps.viewheight, eye );
	VectorSubtract( tr.endpos, eye, dir );
	if ( VectorNormalize( dir ) < 1.0f )
	{
		VectorCopy( forward, dir );		// the spot is at the eye: the hand's direction
	}
	vectoangles( dir, angles );
	ent->s.apos.trBase[PITCH] = AngleNormalize180( angles[PITCH] );
	ent->s.apos.trBase[YAW] = AngleNormalize360( angles[YAW] );
}

// a partner's cgame: where a VR player's entity angles look from (qfalse: a flat player, whose
// beam is drawn as always)
qboolean COOP_VRBeamEye( const gentity_t *ent, const vec3_t origin, vec3_t eye )
{
	vec3_t angles;

	if ( !ent || !ent->client || !COOP_VRAimAngles( ent, angles ) )
	{
		return qfalse;
	}
	CV_EntityEye( origin, ent->client->ps.viewheight, eye );
	return qtrue;
}

// host, in the replica's globals: the VR players' aims (no chunk at all with none)
void COOP_VRWrite( void )
{
	coopVRAim_t	aims[2];
	int			n = 0;
	vec3_t		angles;

	for ( int i = 0; i < 2; i++ )
	{
		const int num = i ? coop_p2Num : 0;

		if ( num < 0 || num >= ENTITYNUM_WORLD || !g_entities[num].inuse || !CV_HostAim( &g_entities[num], angles ) )
		{
			continue;
		}
		aims[n].num = num;
		aims[n].angles[0] = ANGLE2SHORT( angles[PITCH] );
		aims[n].angles[1] = ANGLE2SHORT( angles[YAW] );
		n++;
	}
	if ( n )
	{
		gi.AppendToSaveGame( 'RVRA', aims, n * sizeof( aims[0] ) );
	}
}

// guest: the host's aims (none: nobody aims with a VR hand now)
void COOP_VRRead( void )
{
	coopVRAim_t aims[2];
	int n = gi.ReadFromSaveGameOptional( 'RVRA', aims, sizeof( aims ), NULL ) / (int)sizeof( aims[0] );

	cv_repCount = 0;
	for ( int i = 0; i < n && i < 2; i++ )
	{
		cv_rep[cv_repCount++] = aims[i];
	}
}
