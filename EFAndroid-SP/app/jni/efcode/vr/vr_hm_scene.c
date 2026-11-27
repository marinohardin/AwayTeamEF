/*
===========================================================================
AwayTeamEF VR: Holomatch H1, the first-person gun at the hand (EF_VR only).

This file is part of AwayTeamEF and is licensed under the GNU General
Public License, version 2 or (at your option) any later version.
===========================================================================

Compiled into the engine target (an INTERFACE source of ef_vr). docs/vr/HOLOMATCH-H1.md §4.1
and §4.3.

The Holomatch cgame (cMod's or the stock one) draws its view weapon for a flat camera: the gun,
its barrels and its flash, all marked RF_FIRST_PERSON, in front of the view, which in H1
follows the gun's aim. The engine doesn't touch the cgame; it only adjusts what the cgame
hands to the renderer and the sound system, on this client only:

- CG_R_CLEARSCENE starts a scene. While H1 is active, first-person entities, beams and the
  lights are kept back in a small buffer (VR_HM_SceneAddEntity, VR_HM_SceneAddLight); all
  the rest goes straight to the renderer.
- CG_R_RENDERSCENE of a world scene (VR_HM_SceneRender): the gun G (the first first-person
  model) is moved into the tracked hand: its barrel (the line from its flash point to where
  the game aims it for a flat view) along the hand's ray, the grip (behind and below the middle of the model's bounds, as in single player) in
  the controller, which also gives the wrist roll. The game's raise and lower and its small
  sway stay. The same rigid move T = G' G^-1 is applied to every first-person entity (gun, barrels,
  flash, powerup shells) and to the lights at the flash. RF_DEPTHHACK is cleared: in stereo it
  would draw the gun through walls.
- Beams that start at the gun's flash point (now or in the last half second, for beams the
  cgame keeps alive for a few frames) get their start moved with T: the stock I-MOD's RT_LINE,
  cMod's RT_BEZIER (its control points sheared along with it), the phaser's and the others.
  Their ends are already right: the server's shot goes to where the hand points. Beams, sprites
  (flashes, puffs, smoke) and lights at the server's muzzle, where the cgame draws event effects,
  or a little off either point move with it too. Other models never do: missiles start at the
  server's muzzle and must stay on their path.
- CG_S_RESPATIALIZE: the cgame's listener is its view, which is the aim; the engine puts the
  rendered head there instead, so sounds come from where they are around the head.
Everything else in the scene, and the scene itself, is the cgame's.
*/

#include "../client/client.h"
#include "vr.h"
#include "vr_input.h"
#include "vr_hm.h"

#define VR_HM_MAX_FP		64			// first-person entities in one scene
#define VR_HM_MAX_BEAMS		128
#define VR_HM_MAX_LIGHTS	16
#define VR_HM_FLASH_HISTORY	48			// frames of flash points remembered (about 0.7 s at 72 Hz)
#define VR_HM_FLASH_KEEP	700			// ms
#define VR_HM_BEAM_MATCH	2.0f		// a beam starting this close to a flash point is the gun's ...
#define VR_HM_MUZZLE_MATCH	6.0f		// ... or this close to the server's muzzle (beams the server
										// sends as events start there, rounded to whole units)
#define VR_HM_LIGHT_MATCH	6.0f
#define VR_HM_MAX_NEAR		128			// sprites and models near the gun's muzzle in one scene
#define VR_HM_NEAR_KEEP		32.0f		// kept back when this close to a recent flash point or muzzle ...
#define VR_HM_NEAR_MATCH	12.0f		// ... and moved with the gun when this close (flashes, puffs, smoke)
#define VR_HM_BARREL_AIM	512.0f		// how far ahead of the view the game aims its view weapons [guess]

// Where each Holomatch weapon sits in the hand, added to the model's default grip: forward,
// left, up (units, the hand's frame) and pitch, yaw, roll (degrees). Our own values, tuned
// with vr_hmGunAdjust.
static const float vr_hmGunFit[HM_WP_COUNT][6] = {
	{ 0, 0, 0,	0, 0, 0 },	// none
	{ 0, 0, 0,	0, 0, 0 },	// phaser
	{ 0, 0, 0,	0, 0, 0 },	// compression rifle
	{ 0, 0, 0,	0, 0, 0 },	// I-MOD
	{ 0, 0, 0,	0, 0, 0 },	// scavenger rifle
	{ 0, 0, 0,	0, 0, 0 },	// stasis weapon
	{ 0, 0, 0,	0, 0, 0 },	// grenade launcher
	{ 0, 0, 0,	0, 0, 0 },	// tetryon disruptor
	{ 0, 0, 0,	0, 0, 0 },	// quantum burst
	{ 0, 0, 0,	0, 0, 0 },	// arc welder
	{ 0, 0, 0,	0, 0, 0 },	// hypo
	{ 0, 0, 0,	0, 0, 0 },	// Borg assimilator
	{ 0, 0, 0,	0, 0, 0 },	// Borg weapon
};

typedef struct {
	int		time;
	vec3_t	flash;			// the cgame's flash point (tag_flash of the gun) in the world
	vec3_t	drawnFlash;		// ... moved to the hand: where the gun's beams start now
	vec3_t	muzzle;			// the server's muzzle for the angles sent
	qboolean haveMuzzle;
	vec3_t	gunOrg;			// G's origin
	vec3_t	newOrg;			// G''s origin
	vec3_t	rot[3];			// T's rotation: v' = rot * v (rows)
} vrHmMove_t;

static struct {
	qboolean		registered;
	cvar_t			*gun;
	cvar_t			*gunAdjust;
	cvar_t			*gunAlign;

	qboolean		active;			// H1 was active when the scene began
	refEntity_t		fp[VR_HM_MAX_FP];
	int				numFp;
	refEntity_t		beams[VR_HM_MAX_BEAMS];
	int				numBeams;
	refEntity_t		near[VR_HM_MAX_NEAR];
	int				numNear;
	struct { vec3_t org; float intensity, r, g, b; } lights[VR_HM_MAX_LIGHTS];
	int				numLights;

	vrHmMove_t		hist[VR_HM_FLASH_HISTORY];
	int				histNext;

	int				lastInfo;
} vhs;

static void VR_HM_SceneRegister( void ) {
	vhs.gun = Cvar_Get( "vr_hmGun", "1", CVAR_ARCHIVE );
	Cvar_CheckRange( vhs.gun, 0, 1, qtrue );
	Cvar_SetDescription( vhs.gun, "VR, Holomatch: draw the first-person gun in the tracked hand (1) or where the game puts it (0)." );
	vhs.gunAdjust = Cvar_Get( "vr_hmGunAdjust", "", 0 );
	Cvar_SetDescription( vhs.gunAdjust, "VR, Holomatch: tune the gun in the hand, \"fwd left up pitch yaw roll\" (units, degrees), "
		"added to the weapon's own fit." );
	vhs.gunAlign = Cvar_Get( "vr_hmGunAlign", "1", 0 );
	Cvar_CheckRange( vhs.gunAlign, 0, 1, qtrue );
	Cvar_SetDescription( vhs.gunAlign, "VR, Holomatch: 1 = the gun's barrel along the hand's ray, 0 = keep the turn the game gives "
		"the gun for a flat view (towards the screen's middle)." );
	vhs.registered = qtrue;
}

void VR_HM_SceneClear( void ) {
	if ( !vhs.registered ) {
		VR_HM_SceneRegister();
	}
	vhs.numFp = vhs.numBeams = vhs.numLights = vhs.numNear = 0;
	vhs.active = VR_HM_Active() && vhs.gun->integer ? qtrue : qfalse;
}

static qboolean VR_HM_IsBeam( int reType ) {
	return reType == RT_LINE || reType == RT_ORIENTEDLINE || reType == RT_LINE2 || reType == RT_BEZIER ||
		reType == RT_CYLINDER || reType == RT_ELECTRICITY;
}

static const vrHmMove_t *VR_HM_FindFlash( const vec3_t p, float flashR, float muzzleR, vec3_t refOut );

int VR_HM_SceneAddEntity( const void *entPtr ) {
	const refEntity_t *ent = (const refEntity_t *)entPtr;

	if ( !vhs.active || !ent ) {
		return 0;
	}
	if ( ent->renderfx & RF_FIRST_PERSON ) {
		if ( vhs.numFp >= VR_HM_MAX_FP ) {
			return 0;
		}
		vhs.fp[vhs.numFp++] = *ent;
		return 1;
	}
	if ( VR_HM_IsBeam( ent->reType ) ) {
		if ( vhs.numBeams >= VR_HM_MAX_BEAMS ) {
			return 0;
		}
		vhs.beams[vhs.numBeams++] = *ent;
		return 1;
	}
	// the cgame's flashes, puffs and smoke at the muzzle: sprites and small models it places
	// at the flat view's flash point or at the server's muzzle
	if ( ent->reType == RT_MODEL ) {
		// not moved: missiles start at the server's muzzle and must stay on their path
		if ( VR_HM_DebugLevel() >= 4 && VR_HM_FindFlash( ent->origin, VR_HM_NEAR_KEEP, VR_HM_NEAR_KEEP, NULL ) ) {
			Com_Printf( "vrhm model near: model %d shader %d at %.1f %.1f %.1f\n", ent->hModel, ent->customShader,
				ent->origin[0], ent->origin[1], ent->origin[2] );
		}
		return 0;
	}
	if ( ( ent->reType == RT_SPRITE || ent->reType == RT_ORIENTEDSPRITE || ent->reType == RT_ALPHAVERTPOLY ) &&
			VR_HM_FindFlash( ent->origin, VR_HM_NEAR_KEEP, VR_HM_NEAR_KEEP, NULL ) ) {
		if ( vhs.numNear >= VR_HM_MAX_NEAR ) {
			return 0;
		}
		vhs.near[vhs.numNear++] = *ent;
		return 1;
	}
	return 0;
}

// polys aren't moved; at vr_hmDebug 4 the ones near the muzzle are listed
void VR_HM_ScenePoly( int numVerts, const void *vertsPtr ) {
	const polyVert_t *v = (const polyVert_t *)vertsPtr;
	vec3_t ref;

	if ( !vhs.active || numVerts < 1 || !v || VR_HM_DebugLevel() < 4 ) {
		return;
	}
	if ( VR_HM_FindFlash( v[0].xyz, VR_HM_NEAR_KEEP, VR_HM_NEAR_KEEP, ref ) ) {
		Com_Printf( "vrhm poly near: %d verts at %.1f %.1f %.1f, %.1f from the flash or muzzle\n", numVerts,
			v[0].xyz[0], v[0].xyz[1], v[0].xyz[2], Distance( v[0].xyz, ref ) );
	}
}

int VR_HM_SceneAddLight( const float *org, float intensity, float r, float g, float b ) {
	if ( !vhs.active || vhs.numLights >= VR_HM_MAX_LIGHTS ) {
		return 0;
	}
	VectorCopy( org, vhs.lights[vhs.numLights].org );
	vhs.lights[vhs.numLights].intensity = intensity;
	vhs.lights[vhs.numLights].r = r;
	vhs.lights[vhs.numLights].g = g;
	vhs.lights[vhs.numLights].b = b;
	vhs.numLights++;
	return 1;
}

static void VR_HM_SceneFlush( void ) {
	int i;

	for ( i = 0; i < vhs.numFp; i++ ) {
		re.AddRefEntityToScene( &vhs.fp[i] );
	}
	for ( i = 0; i < vhs.numBeams; i++ ) {
		re.AddRefEntityToScene( &vhs.beams[i] );
	}
	for ( i = 0; i < vhs.numNear; i++ ) {
		re.AddRefEntityToScene( &vhs.near[i] );
	}
	for ( i = 0; i < vhs.numLights; i++ ) {
		re.AddLightToScene( vhs.lights[i].org, vhs.lights[i].intensity, vhs.lights[i].r, vhs.lights[i].g, vhs.lights[i].b );
	}
	vhs.numFp = vhs.numBeams = vhs.numLights = vhs.numNear = 0;
}

// p' = newOrg + rot * ( p - gunOrg )
static void VR_HM_MovePoint( const vrHmMove_t *m, const vec3_t in, vec3_t out ) {
	vec3_t d;
	int i;

	VectorSubtract( in, m->gunOrg, d );
	for ( i = 0; i < 3; i++ ) {
		out[i] = m->newOrg[i] + DotProduct( m->rot[i], d );
	}
}

static void VR_HM_MoveVector( const vrHmMove_t *m, const vec3_t in, vec3_t out ) {
	vec3_t t;
	int i;

	for ( i = 0; i < 3; i++ ) {
		t[i] = DotProduct( m->rot[i], in );
	}
	VectorCopy( t, out );
}

static void VR_HM_MoveEntity( const vrHmMove_t *m, refEntity_t *e ) {
	int i;

	VR_HM_MovePoint( m, e->origin, e->origin );
	VR_HM_MovePoint( m, e->oldorigin, e->oldorigin );
	VR_HM_MovePoint( m, e->lightingOrigin, e->lightingOrigin );
	for ( i = 0; i < 3; i++ ) {
		VR_HM_MoveVector( m, e->axis[i], e->axis[i] );
	}
	e->renderfx &= ~RF_DEPTHHACK;
}

// works out T for this scene from the gun; 0 when there is nothing to move
static int VR_HM_GunMove( const refdef_t *fd, vrHmMove_t *m ) {
	const refEntity_t *g = NULL;
	vec3_t handOrg, handAxis[3], rel[3], adj[3], ctrl[3], newAxis[3], mins, maxs, mid, grip, angles;
	float fit[6], tune[6] = { 0, 0, 0, 0, 0, 0 };
	orientation_t tag;
	int i, j, weapon;

	for ( i = 0; i < vhs.numFp; i++ ) {
		if ( vhs.fp[i].reType == RT_MODEL && vhs.fp[i].hModel ) {
			g = &vhs.fp[i];
			break;
		}
	}
	if ( !g || !VR_HM_HandPose( fd->vieworg, handOrg, handAxis ) ) {
		return 0;
	}

	// the cgame's flash point: the gun's tag_flash, where its beams start
	Com_Memset( &tag, 0, sizeof( tag ) );
	VectorCopy( g->origin, m->flash );
	if ( re.LerpTag( &tag, g->hModel, g->oldframe, g->frame, 1.0f - g->backlerp, "tag_flash" ) ) {
		for ( i = 0; i < 3; i++ ) {
			VectorMA( m->flash, tag.origin[i], g->axis[i], m->flash );
		}
	}

	// the gun's rotation relative to the cgame's view. For a flat view the game turns the gun
	// so its barrel points at the middle of the screen, from where the gun sits (lower right).
	// The models' own axes differ from weapon to weapon, so that turn is the best guide to
	// where each barrel points: turn the gun once more so the line from its flash point to a
	// point straight ahead of the view lies along the view's forward axis. In the hand, that
	// line then lies along the hand's ray.
	for ( i = 0; i < 3; i++ ) {
		for ( j = 0; j < 3; j++ ) {
			rel[i][j] = DotProduct( g->axis[i], fd->viewaxis[j] );
		}
	}
	if ( vhs.gunAlign->integer ) {
		vec3_t fv, b, x = { 1, 0, 0 }, ax, t;
		float c, sn, ang;

		VectorSubtract( m->flash, fd->vieworg, t );
		for ( j = 0; j < 3; j++ ) {
			fv[j] = DotProduct( t, fd->viewaxis[j] );
		}
		VectorSet( b, VR_HM_BARREL_AIM - fv[0], -fv[1], -fv[2] );
		VectorNormalize( b );
		CrossProduct( b, x, ax );
		sn = VectorNormalize( ax );
		c = DotProduct( b, x );
		ang = atan2f( sn, c );
		if ( sn > 1e-5f ) {
			for ( i = 0; i < 3; i++ ) {
				vec3_t v;
				RotatePointAroundVector( v, ax, rel[i], RAD2DEG( ang ) );
				VectorCopy( v, rel[i] );
			}
		}
	}

	// the fit: the table, then the live tuning
	weapon = cl.snap.ps.weapon;
	for ( i = 0; i < 6; i++ ) {
		fit[i] = ( weapon > 0 && weapon < HM_WP_COUNT ) ? vr_hmGunFit[weapon][i] : 0.0f;
	}
	if ( vhs.gunAdjust->string[0] ) {
		sscanf( vhs.gunAdjust->string, "%f %f %f %f %f %f", &tune[0], &tune[1], &tune[2], &tune[3], &tune[4], &tune[5] );
		for ( i = 0; i < 6; i++ ) {
			fit[i] += tune[i];
		}
	}
	if ( Cvar_VariableIntegerValue( "vr_leftHanded" ) ) {
		fit[1] = -fit[1];		// no mirrored models: the same model, held from the other side
		fit[4] = -fit[4];
		fit[5] = -fit[5];
	}
	VectorSet( angles, fit[3], fit[4], fit[5] );
	AnglesToAxis( angles, adj );
	MatrixMultiply( adj, handAxis, ctrl );

	// G' axes: the gun's own rotation, carried by the controller's frame
	for ( i = 0; i < 3; i++ ) {
		VectorClear( newAxis[i] );
		for ( j = 0; j < 3; j++ ) {
			VectorMA( newAxis[i], rel[i][j], ctrl[j], newAxis[i] );
		}
	}

	// the grip, in the gun's model space: behind and below the middle of its bounds
	re.ModelBounds( g->hModel, mins, maxs );
	for ( i = 0; i < 3; i++ ) {
		mid[i] = 0.5f * ( mins[i] + maxs[i] );
	}
	mid[0] -= 0.25f * ( maxs[0] - mins[0] );
	mid[2] -= 0.25f * ( maxs[2] - mins[2] );
	VectorCopy( handOrg, m->newOrg );
	for ( i = 0; i < 3; i++ ) {
		VectorMA( m->newOrg, -mid[i], newAxis[i], m->newOrg );
	}
	VectorSet( grip, fit[0], fit[1], fit[2] );
	for ( i = 0; i < 3; i++ ) {
		VectorMA( m->newOrg, -grip[i], ctrl[i], m->newOrg );
	}

	// T's rotation: rot = newAxis^T-columns times G's axes; v' = sum_k ( v . g_k ) newAxis_k
	for ( i = 0; i < 3; i++ ) {
		for ( j = 0; j < 3; j++ ) {
			m->rot[i][j] = g->axis[0][j] * newAxis[0][i] + g->axis[1][j] * newAxis[1][i] + g->axis[2][j] * newAxis[2][i];
		}
	}
	VectorCopy( g->origin, m->gunOrg );

	m->time = cl.serverTime;
	m->haveMuzzle = VR_HM_ServerMuzzle( m->muzzle ) ? qtrue : qfalse;

	if ( VR_HM_DebugLevel() >= 4 && Sys_Milliseconds() - vhs.lastInfo > 1000 ) {
		vhs.lastInfo = Sys_Milliseconds();
		Com_Printf( "vrhm gun: w %d fp %d beams %d lights %d model %d rel fwd %.3f %.3f %.3f up %.3f %.3f %.3f grip %.1f %.1f %.1f flash %.1f %.1f %.1f\n",
			weapon, vhs.numFp, vhs.numBeams, vhs.numLights, g->hModel, rel[0][0], rel[0][1], rel[0][2], rel[2][0], rel[2][1], rel[2][2],
			mid[0], mid[1], mid[2], m->flash[0], m->flash[1], m->flash[2] );
		for ( i = 0; i < vhs.numFp; i++ ) {
			Com_Printf( "vrhm fp %d: type %d fx 0x%x model %d shader %d org %.1f %.1f %.1f\n", i, vhs.fp[i].reType,
				vhs.fp[i].renderfx, vhs.fp[i].hModel, vhs.fp[i].customShader, vhs.fp[i].origin[0], vhs.fp[i].origin[1], vhs.fp[i].origin[2] );
		}
	}
	return 1;
}

// the frame whose flash point (within flashR) or server muzzle (within muzzleR) is nearest
// to p, or NULL; refOut gets that flash point or muzzle
static const vrHmMove_t *VR_HM_FindFlash( const vec3_t p, float flashR, float muzzleR, vec3_t refOut ) {
	const vrHmMove_t *best = NULL;
	const float *bestRef = NULL;
	float bestD = 1.0e9f;
	int i;

	for ( i = 0; i < VR_HM_FLASH_HISTORY; i++ ) {
		const vrHmMove_t *h = &vhs.hist[i];
		float d;

		if ( !h->time || cl.serverTime - h->time > VR_HM_FLASH_KEEP || h->time > cl.serverTime ) {
			continue;
		}
		d = Distance( p, h->flash );
		if ( d <= flashR && d < bestD ) {
			bestD = d;
			best = h;
			bestRef = h->flash;
		}
		if ( h->haveMuzzle ) {
			d = Distance( p, h->muzzle );
			if ( d <= muzzleR && d < bestD ) {
				bestD = d;
				best = h;
				bestRef = h->muzzle;
			}
		}
	}
	if ( best && refOut ) {
		VectorCopy( bestRef, refOut );
	}
	return best;
}

void VR_HM_SceneRender( const void *refdefPtr ) {
	const refdef_t *fd = (const refdef_t *)refdefPtr;
	vrHmMove_t move;
	int i, haveMove;

	if ( !vhs.registered ) {
		VR_HM_SceneRegister();
	}
	if ( !vhs.active || !fd || ( fd->rdflags & RDF_NOWORLDMODEL ) ) {
		VR_HM_SceneFlush();
		VR_HM_Scene( refdefPtr, NULL );
		return;
	}

	haveMove = VR_HM_GunMove( fd, &move );
	if ( haveMove ) {
		VR_HM_MovePoint( &move, move.flash, move.drawnFlash );
		vhs.hist[vhs.histNext] = move;
		vhs.histNext = ( vhs.histNext + 1 ) % VR_HM_FLASH_HISTORY;
		for ( i = 0; i < vhs.numFp; i++ ) {
			VR_HM_MoveEntity( &move, &vhs.fp[i] );
		}
	}
	// lights at a recent flash point or muzzle: moved by as much as that point
	for ( i = 0; i < vhs.numLights; i++ ) {
		vec3_t ref, d;
		const vrHmMove_t *m = VR_HM_FindFlash( vhs.lights[i].org, VR_HM_LIGHT_MATCH, VR_HM_NEAR_MATCH, ref );

		if ( m ) {
			VectorSubtract( m->drawnFlash, ref, d );
			VectorAdd( vhs.lights[i].org, d, vhs.lights[i].org );
		}
	}

	// beams from the gun's flash point, this frame or a recent one: move their start
	for ( i = 0; i < vhs.numBeams; i++ ) {
		refEntity_t *b = &vhs.beams[i];
		vec3_t start, d, ref;
		const vrHmMove_t *m = VR_HM_FindFlash( b->origin, VR_HM_BEAM_MATCH, VR_HM_MUZZLE_MATCH, ref );

		if ( !m ) {
			// beams the cgame starts from the player model's own gun (the arc welder's) or
			// a little off the flash point: near enough, it's this gun's
			m = VR_HM_FindFlash( b->origin, VR_HM_NEAR_MATCH, VR_HM_NEAR_MATCH, ref );
		}

		if ( !m ) {
			if ( VR_HM_DebugLevel() >= 4 ) {
				Com_Printf( "vrhm beam kept: type %d start %.1f %.1f %.1f end %.1f %.1f %.1f\n", b->reType,
					b->origin[0], b->origin[1], b->origin[2], b->oldorigin[0], b->oldorigin[1], b->oldorigin[2] );
			}
			continue;
		}
		VectorCopy( m->drawnFlash, start );
		if ( VR_HM_DebugLevel() >= 4 ) {
			Com_Printf( "vrhm beam moved: type %d age %d off %.1f start %.1f %.1f %.1f -> %.1f %.1f %.1f end %.1f %.1f %.1f\n", b->reType,
				cl.serverTime - m->time, Distance( b->origin, ref ), b->origin[0], b->origin[1], b->origin[2], start[0], start[1], start[2],
				b->oldorigin[0], b->oldorigin[1], b->oldorigin[2] );
		}
		VectorSubtract( start, b->origin, d );
		VectorCopy( start, b->origin );
		if ( b->reType == RT_BEZIER ) {
			// shear the curve with its start, so it keeps its shape towards the end
			VectorMA( b->data.bezier.control1, 2.0f / 3.0f, d, b->data.bezier.control1 );
			VectorMA( b->data.bezier.control2, 1.0f / 3.0f, d, b->data.bezier.control2 );
		}
	}

	// flashes, puffs and smoke at a recent flash point or muzzle: moved by as much as that point
	for ( i = 0; i < vhs.numNear; i++ ) {
		refEntity_t *e = &vhs.near[i];
		vec3_t ref, d;
		const vrHmMove_t *m = VR_HM_FindFlash( e->origin, VR_HM_NEAR_MATCH, VR_HM_NEAR_MATCH, ref );

		if ( !m ) {
			if ( VR_HM_DebugLevel() >= 4 ) {
				Com_Printf( "vrhm near kept: type %d shader %d model %d at %.1f %.1f %.1f\n", e->reType, e->customShader,
					e->hModel, e->origin[0], e->origin[1], e->origin[2] );
			}
			continue;
		}
		VectorSubtract( m->drawnFlash, ref, d );
		if ( VR_HM_DebugLevel() >= 4 ) {
			Com_Printf( "vrhm near moved: type %d shader %d model %d age %d off %.1f by %.1f %.1f %.1f\n", e->reType,
				e->customShader, e->hModel, cl.serverTime - m->time, Distance( e->origin, ref ), d[0], d[1], d[2] );
		}
		VectorAdd( e->origin, d, e->origin );
		VectorAdd( e->oldorigin, d, e->oldorigin );
		VectorAdd( e->lightingOrigin, d, e->lightingOrigin );
	}

	VR_HM_SceneFlush();
	if ( haveMove ) {
		VR_HM_Scene( refdefPtr, move.drawnFlash );	// the laser starts at the drawn gun's flash point
	} else {
		VR_HM_Scene( refdefPtr, NULL );
	}
}

void VR_HM_Respatialize( int entityNum, const float *origin, float axisIn[3][3], int inwater ) {
	vrBodyHand_t head;
	vec3_t org, axis[3];
	float yaw;
	int i;

	if ( !VR_HM_Active() || !origin || !VR_GetBodyYaw( &yaw ) || !VR_EngineBodyHand( VR_BODY_HEAD, &head ) ) {
		S_Respatialize( entityNum, origin, axisIn, inwater );
		return;
	}
	{
		const vrHmState_t st = VR_HM_State();
		if ( st == VR_HM_FOLLOW || st == VR_HM_INTERMISSION ) {
			S_Respatialize( entityNum, origin, axisIn, inwater );
			return;
		}
	}
	{
		const float a = DEG2RAD( yaw ), c = cosf( a ), s = sinf( a );
		org[0] = origin[0] + c * head.origin[0] - s * head.origin[1];
		org[1] = origin[1] + s * head.origin[0] + c * head.origin[1];
		org[2] = origin[2] + head.origin[2];
		for ( i = 0; i < 3; i++ ) {
			axis[i][0] = c * head.axis[i][0] - s * head.axis[i][1];
			axis[i][1] = s * head.axis[i][0] + c * head.axis[i][1];
			axis[i][2] = head.axis[i][2];
		}
	}
	S_Respatialize( entityNum, org, axis, inwater );
}
