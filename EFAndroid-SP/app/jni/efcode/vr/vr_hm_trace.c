/*
===========================================================================
AwayTeamEF VR: Holomatch H1, the engine's trace through the snapshot world (EF_VR only).

This file is part of AwayTeamEF and is licensed under the GNU General
Public License, version 2 or (at your option) any later version.
===========================================================================

Compiled into the engine target (an INTERFACE source of ef_vr). docs/vr/HOLOMATCH-H1.md §3.2.

The aim needs the first thing along the hand's ray, as the player sees it. That is what the
cgame's own trace does, but the cgame is the server's (cMod's or the stock one) and can't be
asked. So the engine traces itself, with the data the cgame has:
- the world (inline model 0);
- every solid entity of the snapshots: inline models (movers, doors, platforms) through their
  own brushes, everything else (players) as a box decoded from entityState.solid;
- at the positions the cgame draws them: interpolated between the two snapshots around
  cl.serverTime, or the entity's trajectory evaluated at that time.
Trigger brushes aren't solid to a shot mask, so they drop out by their contents.
*/

#include "../client/client.h"
#include "vr_hm.h"

#define VR_HM_MAX_SOLIDS	256

typedef struct {
	int				number;
	qboolean		bmodel;
	int				modelindex;
	vec3_t			origin;
	vec3_t			angles;
	vec3_t			mins, maxs;
} vrHmSolid_t;

static struct {
	int				serverTime;		// the list below is for this cl.serverTime ...
	int				messageNum;		// ... and this newest snapshot
	int				count;
	vrHmSolid_t		solids[VR_HM_MAX_SOLIDS];
} vht = { -1, -1 };

// a trajectory's position at a time (ms), the way every Holomatch client evaluates one
static void VR_HM_EvalTrajectory( const trajectory_t *tr, int atTime, vec3_t out ) {
	float dt, phase;

	switch ( tr->trType ) {
	case TR_LINEAR:
		dt = ( atTime - tr->trTime ) * 0.001f;
		VectorMA( tr->trBase, dt, tr->trDelta, out );
		break;
	case TR_LINEAR_STOP:
		if ( atTime > tr->trTime + tr->trDuration ) {
			atTime = tr->trTime + tr->trDuration;
		}
		dt = ( atTime - tr->trTime ) * 0.001f;
		if ( dt < 0 ) {
			dt = 0;
		}
		VectorMA( tr->trBase, dt, tr->trDelta, out );
		break;
	case TR_SINE:
		phase = tr->trDuration ? (float)( atTime - tr->trTime ) / (float)tr->trDuration : 0.0f;
		VectorMA( tr->trBase, (float)sin( phase * M_PI * 2 ), tr->trDelta, out );
		break;
	case TR_GRAVITY:
		dt = ( atTime - tr->trTime ) * 0.001f;
		VectorMA( tr->trBase, dt, tr->trDelta, out );
		out[2] -= 0.5f * 800.0f * dt * dt;
		break;
	default:	// TR_STATIONARY, TR_INTERPOLATE
		VectorCopy( tr->trBase, out );
		break;
	}
}

// the snapshots on either side of cl.serverTime, as the cgame interpolates between them
static void VR_HM_SnapPair( const clSnapshot_t **prev, const clSnapshot_t **next ) {
	int i;

	*prev = *next = NULL;
	for ( i = 0; i < PACKET_BACKUP - 1; i++ ) {
		const clSnapshot_t *s = &cl.snapshots[( cl.snap.messageNum - i ) & PACKET_MASK];

		if ( !s->valid || s->messageNum != cl.snap.messageNum - i ) {
			break;
		}
		if ( cl.parseEntitiesNum - s->parseEntitiesNum >= MAX_PARSE_ENTITIES ) {
			break;	// its entities are overwritten
		}
		if ( s->serverTime <= cl.serverTime ) {
			*prev = s;
			return;
		}
		*next = s;
	}
	if ( !*prev ) {
		*prev = *next ? *next : &cl.snap;
		*next = NULL;
	}
}

static const entityState_t *VR_HM_FindInSnap( const clSnapshot_t *s, int number ) {
	int i;

	for ( i = 0; i < s->numEntities; i++ ) {
		const entityState_t *es = &cl.parseEntities[( s->parseEntitiesNum + i ) & ( MAX_PARSE_ENTITIES - 1 )];
		if ( es->number == number ) {
			return es;
		}
	}
	return NULL;
}

static void VR_HM_BuildSolids( void ) {
	const clSnapshot_t *prev, *next;
	float f = 0.0f;
	int i;

	if ( vht.serverTime == cl.serverTime && vht.messageNum == cl.snap.messageNum ) {
		return;
	}
	vht.serverTime = cl.serverTime;
	vht.messageNum = cl.snap.messageNum;
	vht.count = 0;

	VR_HM_SnapPair( &prev, &next );
	if ( next && next->serverTime > prev->serverTime ) {
		f = (float)( cl.serverTime - prev->serverTime ) / (float)( next->serverTime - prev->serverTime );
		f = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
	}

	for ( i = 0; i < prev->numEntities && vht.count < VR_HM_MAX_SOLIDS; i++ ) {
		const entityState_t *es = &cl.parseEntities[( prev->parseEntitiesNum + i ) & ( MAX_PARSE_ENTITIES - 1 )];
		const entityState_t *en;
		vrHmSolid_t *s;

		if ( !es->solid ) {
			continue;
		}
		s = &vht.solids[vht.count++];
		s->number = es->number;
		en = next ? VR_HM_FindInSnap( next, es->number ) : NULL;

		if ( en && es->pos.trType == TR_INTERPOLATE ) {
			vec3_t a, b;
			VR_HM_EvalTrajectory( &es->pos, prev->serverTime, a );
			VR_HM_EvalTrajectory( &en->pos, next->serverTime, b );
			s->origin[0] = a[0] + f * ( b[0] - a[0] );
			s->origin[1] = a[1] + f * ( b[1] - a[1] );
			s->origin[2] = a[2] + f * ( b[2] - a[2] );
		} else {
			VR_HM_EvalTrajectory( &es->pos, cl.serverTime, s->origin );
		}

		if ( es->solid == SOLID_BMODEL ) {
			s->bmodel = qtrue;
			s->modelindex = es->modelindex;
			if ( en && es->apos.trType == TR_INTERPOLATE ) {
				vec3_t a, b;
				VR_HM_EvalTrajectory( &es->apos, prev->serverTime, a );
				VR_HM_EvalTrajectory( &en->apos, next->serverTime, b );
				s->angles[0] = LerpAngle( a[0], b[0], f );
				s->angles[1] = LerpAngle( a[1], b[1], f );
				s->angles[2] = LerpAngle( a[2], b[2], f );
			} else {
				VR_HM_EvalTrajectory( &es->apos, cl.serverTime, s->angles );
			}
		} else {
			// the box packed by the server: x/y half-width, z down, z up (+32)
			const int x = es->solid & 255;
			const int zd = ( es->solid >> 8 ) & 255;
			const int zu = ( ( es->solid >> 16 ) & 255 ) - 32;

			s->bmodel = qfalse;
			VectorSet( s->mins, -x, -x, -zd );
			VectorSet( s->maxs, x, x, zu );
			VectorClear( s->angles );
		}
	}
}

void VR_HM_Trace( vrHmTrace_t *out, const float start[3], const float end[3],
		const float *mins, const float *maxs, int skip, int mask ) {
	vec3_t bmins = { 0, 0, 0 }, bmaxs = { 0, 0, 0 };
	trace_t best, tr;
	int i;

	if ( mins && maxs ) {
		VectorCopy( mins, bmins );
		VectorCopy( maxs, bmaxs );
	}
	VR_HM_BuildSolids();

	CM_BoxTrace( &best, start, end, bmins, bmaxs, 0, mask, qfalse );
	best.entityNum = best.fraction < 1.0f ? ENTITYNUM_WORLD : ENTITYNUM_NONE;

	for ( i = 0; i < vht.count && !best.allsolid; i++ ) {
		const vrHmSolid_t *s = &vht.solids[i];
		clipHandle_t model;

		if ( s->number == skip ) {
			continue;
		}
		if ( s->bmodel ) {
			model = CM_InlineModel( s->modelindex );
		} else {
			model = CM_TempBoxModel( s->mins, s->maxs, qfalse );
		}
		CM_TransformedBoxTrace( &tr, start, end, bmins, bmaxs, model, mask, s->origin, s->angles, qfalse );
		if ( tr.allsolid || tr.fraction < best.fraction ) {
			tr.entityNum = s->number;
			best = tr;
		} else if ( tr.startsolid ) {
			best.startsolid = qtrue;
		}
	}

	out->fraction = best.fraction;
	VectorCopy( best.endpos, out->endpos );
	VectorCopy( best.plane.normal, out->normal );
	out->entityNum = ( best.entityNum == ENTITYNUM_WORLD || best.entityNum == ENTITYNUM_NONE ) ? -1 : best.entityNum;
	out->startsolid = best.startsolid;
}
