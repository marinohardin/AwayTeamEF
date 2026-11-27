/*
===========================================================================
AwayTeamEF VR: Holomatch H1, the head: the small lean, the walk after the head, the fade and
the crouch height (EF_VR only).

This file is part of AwayTeamEF and is licensed under the GNU General
Public License, version 2 or (at your option) any later version.
===========================================================================

Compiled into the engine target (an INTERFACE source of ef_vr). docs/vr/HOLOMATCH-H1.md §5.

On someone else's server the body only moves through the usercmd, and nothing may give a VR
player an unseen advantage. So, while H1 is active (vr_move.c hands its Holomatch branch to
VR_HM_Head each frame):

- The eye may be at most vr_hmLeanRadius units (default 4, about 12 cm) from the body's centre,
  horizontally: inside the head other players see. The view is never placed further out.
- Past that, the excess is a step debt. Each command turns it into forwardmove/rightmove
  (VR_HM_HeadCmd, about 8 per second of the debt, at most 64 so the walk button stays set and
  the steps are silent), so the body walks after the head where everyone can see it. The head
  origin (the anchor) moves by what the body really achieved: the cgame's eye moving along the
  step's direction.
- Where the body can't follow, the eye stays at the radius and the view fades (black about
  12 cm further): a wall or another player in the way, no floor at the step (a ledge: leaning
  out over a drop never throws the player off it), in the air, a menu or the console up, dead,
  spectating, vr_hmRoomScale 0.
- While the move stick is used, the head origin is simply dragged along (the stick moves the
  body; the eye still stays inside the radius).
- A small box from the body's eye to the rendered eye keeps the eye out of walls in corners.
- Height: the cgame already lowers its eye for a crouch, so the real head's drop counts only
  below the game's own: eye = min( game eye, standing eye + real drop ). The eye is never higher
  than the game's eye (a crouched hitbox never peeks from standing height).
*/

#include "../client/client.h"
#include "vr.h"
#include "vr_input.h"
#include "vr_hm.h"

#define VR_HM_STEP_GAIN		8.0f		// per second: the step debt's speed
#define VR_HM_STEP_MAX		64			// the walk button's limit in the game (silent steps)
#define VR_HM_SPEED			250.0f		// the game's default run speed (g_speed), units per second
#define VR_HM_FADE_START	0.03f		// metres past the radius where the fade starts
#define VR_HM_FADE_RANGE	0.12f		// ... and how much further it is black
#define VR_HM_LEDGE_DROP	26.0f		// a step height (18) plus 8: deeper than that is a ledge
#define VR_HM_DUCK_RATE		200.0f		// units per second, the crouch drop's ease
#define HM_ENTITYNUM_NONE	1023
#define VR_HM_MASK_PLAYER	( CONTENTS_SOLID | CONTENTS_PLAYERCLIP | CONTENTS_BODY )

static struct {
	qboolean	registered;
	cvar_t		*radius;
	cvar_t		*roomScale;
	cvar_t		*worldScale;

	qboolean	havePrev;
	vec3_t		prevView;		// the cgame's eye last frame
	vec3_t		stepDir;		// world, unit: the direction the debt asked for (zero = none)
	float		debt;			// units still asked for along stepDir
	vec3_t		debtWorld;		// the debt for the next command, world units

	float		standVH;		// the game's standing view height (the largest seen)
	float		duck;			// units: the game's crouch drop, eased

	// for the logs (vr_hmDebug)
	float		eyeOfs[3];		// the rendered eye minus the cgame's eye (world units)
	float		fade;
	int			reason;
	int			lastPrint;
} vhh;

enum { HH_OK, HH_STICK, HH_WALL, HH_LEDGE, HH_AIR, HH_UI, HH_STATE, HH_OFF };
static const char *vr_hhReason[] = { "ok", "stick", "wall", "ledge", "air", "ui", "state", "off" };

static void VR_HM_HeadRegister( void ) {
	vhh.radius = Cvar_Get( "vr_hmLeanRadius", "4", CVAR_ARCHIVE );
	Cvar_CheckRange( vhh.radius, 1.0f, 4.0f, qfalse );
	Cvar_SetDescription( vhh.radius, "VR, Holomatch: game units the eye may move from the body's centre (inside the drawn head). "
		"Further, the body walks after the head, or the view fades where it can't." );
	vhh.roomScale = Cvar_Get( "vr_hmRoomScale", "1", CVAR_ARCHIVE );
	Cvar_CheckRange( vhh.roomScale, 0, 1, qtrue );
	Cvar_SetDescription( vhh.roomScale, "VR, Holomatch: 1 = stepping in the room walks the player, 0 = the head only leans "
		"within vr_hmLeanRadius and the view fades past it (seated play)." );
	vhh.worldScale = Cvar_Get( "vr_worldScale", "32", 0 );
	vhh.standVH = 26.0f;
	vhh.registered = qtrue;
}

void VR_HM_HeadReset( void ) {
	vhh.havePrev = qfalse;
	vhh.debt = 0.0f;
	VectorClear( vhh.stepDir );
	VectorClear( vhh.debtWorld );
	vhh.duck = 0.0f;
}

// room (x, z, metres) -> world (units), turned by the body yaw; and back
static void VR_HM_RoomToWorld( float x, float z, float yawDeg, float scale, vec3_t out ) {
	const float a = DEG2RAD( yawDeg ), c = cosf( a ), s = sinf( a );
	const float f = -z * scale, l = -x * scale;

	out[0] = c * f - s * l;
	out[1] = s * f + c * l;
	out[2] = 0.0f;
}

static void VR_HM_WorldToRoom( const vec3_t w, float yawDeg, float scale, float *x, float *z ) {
	const float a = DEG2RAD( yawDeg ), c = cosf( a ), s = sinf( a );
	const float f = c * w[0] + s * w[1], l = -s * w[0] + c * w[1];

	*x = -l / scale;
	*z = -f / scale;
}

// can the body step by `step` (world units, horizontal) from where it is? HH_OK, HH_WALL or HH_LEDGE
static int VR_HM_StepCheck( const vec3_t viewOrg, const vec3_t step ) {
	const playerState_t *ps = &cl.snap.ps;
	static const vec3_t mins = { -15, -15, -6 }, maxs = { 15, 15, 24 };
	static const vec3_t fmins = { -15, -15, -24 }, fmaxs = { 15, 15, 16 };
	vec3_t origin, target, down;
	vrHmTrace_t tr;
	float len;

	VectorCopy( viewOrg, origin );
	origin[2] -= ps->viewheight;
	len = VectorLength( step );
	if ( len < 0.01f ) {
		return HH_OK;
	}
	// look a little ahead of the step (at least half a box), so the check comes before the move
	VectorScale( step, ( len < 8.0f ? 8.0f : len ) / len, target );
	VectorAdd( origin, target, target );

	VR_HM_Trace( &tr, origin, target, mins, maxs, ps->clientNum, VR_HM_MASK_PLAYER );
	if ( tr.fraction < 1.0f || tr.startsolid ) {
		return HH_WALL;
	}
	VectorCopy( target, down );
	down[2] -= VR_HM_LEDGE_DROP;
	VR_HM_Trace( &tr, target, down, fmins, fmaxs, ps->clientNum, VR_HM_MASK_PLAYER );
	if ( tr.fraction >= 1.0f && !tr.startsolid ) {
		return HH_LEDGE;
	}
	return HH_OK;
}

void VR_HM_Head( const float head[3], float anchor[3], int stickActive, int gameInput, float dt,
	float originOut[3], float *fadeOut ) {
	const playerState_t *ps = &cl.snap.ps;
	vec3_t viewOrg, excessW, eyeW, end;
	float scale, r, ox, oz, d, yaw, h, ofsY, fade = 0.0f;
	int reason = HH_OK;

	if ( !vhh.registered ) {
		VR_HM_HeadRegister();
	}
	scale = vhh.worldScale->value < 1.0f ? 1.0f : vhh.worldScale->value;
	r = vhh.radius->value / scale;
	VectorClear( vhh.debtWorld );

	if ( !VR_HM_ViewOrigin( viewOrg ) || !VR_GetBodyYaw( &yaw ) ) {
		// no view yet: the eye at the body's centre, standing height
		originOut[0] = head[0];
		originOut[1] = anchor[1];
		originOut[2] = head[2];
		anchor[0] = head[0];
		anchor[2] = head[2];
		*fadeOut = 0.0f;
		vhh.havePrev = qfalse;
		return;
	}

	// what the body achieved since last frame, along the step asked for (the stick idle)
	if ( vhh.havePrev && vhh.debt > 0.0f && !stickActive ) {
		vec3_t delta;
		float along;

		VectorSubtract( viewOrg, vhh.prevView, delta );
		delta[2] = 0.0f;
		if ( VectorLength( delta ) < 64.0f ) {	// not a teleport or a respawn
			along = DotProduct( delta, vhh.stepDir );
			if ( along > 0.0f ) {
				vec3_t moved;
				float x, z;

				if ( along > vhh.debt + 2.0f ) {
					along = vhh.debt + 2.0f;
				}
				VectorScale( vhh.stepDir, along, moved );
				VR_HM_WorldToRoom( moved, yaw, scale, &x, &z );
				anchor[0] += x;
				anchor[2] += z;
			}
		}
	}
	VectorCopy( viewOrg, vhh.prevView );
	vhh.havePrev = qtrue;
	vhh.debt = 0.0f;
	VectorClear( vhh.stepDir );

	// the head's offset from the anchor, horizontally
	ox = head[0] - anchor[0];
	oz = head[2] - anchor[2];
	d = sqrtf( ox * ox + oz * oz );
	if ( d > r ) {
		const float k = 1.0f - r / d;
		const vrHmState_t st = VR_HM_State();

		VR_HM_RoomToWorld( ox * k, oz * k, yaw, scale, excessW );
		if ( stickActive ) {
			reason = HH_STICK;
		} else if ( !vhh.roomScale->integer ) {
			reason = HH_OFF;
		} else if ( !gameInput ) {
			reason = HH_UI;
		} else if ( st != VR_HM_PLAY ) {
			reason = HH_STATE;
		} else if ( ps->groundEntityNum == HM_ENTITYNUM_NONE ) {
			reason = HH_AIR;
		} else {
			reason = VR_HM_StepCheck( viewOrg, excessW );
		}

		if ( reason == HH_STICK ) {
			anchor[0] += ox * k;		// the stick moves the body; the eye stays within the radius
			anchor[2] += oz * k;
		} else if ( reason == HH_OK ) {
			VectorCopy( excessW, vhh.debtWorld );
			vhh.debt = VectorNormalize2( excessW, vhh.stepDir );
		} else {
			const float past = ( d - r ) - ( VR_HM_FADE_START + VR_HM_FADE_RANGE );
			fade = ( d - r - VR_HM_FADE_START ) / VR_HM_FADE_RANGE;
			fade = fade < 0.0f ? 0.0f : fade > 1.0f ? 1.0f : fade;
			if ( past > 0.0f ) {
				// already black: drag the rest, so stepping back brings the view back at once
				anchor[0] += ox * past / d;
				anchor[2] += oz * past / d;
			}
		}
		ox = head[0] - anchor[0];
		oz = head[2] - anchor[2];
		d = sqrtf( ox * ox + oz * oz );
	}
	if ( d > r ) {
		ox *= r / d;	// the eye never goes past the radius
		oz *= r / d;
	}

	// height: the game's crouch drop, eased (from the snapshot's view height)
	if ( ps->viewheight > vhh.standVH ) {
		vhh.standVH = ps->viewheight;
	}
	{
		const float target = vhh.standVH - ps->viewheight;
		const float stepAmt = VR_HM_DUCK_RATE * dt;
		if ( vhh.duck < target ) {
			vhh.duck = vhh.duck + stepAmt > target ? target : vhh.duck + stepAmt;
		} else if ( vhh.duck > target ) {
			vhh.duck = vhh.duck - stepAmt < target ? target : vhh.duck - stepAmt;
		}
	}
	h = ( head[1] - anchor[1] ) * scale;		// the real head's drop (negative) or rise, units
	ofsY = h + vhh.duck;
	if ( ofsY > 0.0f ) {
		ofsY = 0.0f;		// never above the game's eye
	}

	// the eye in the world; a small box from the body's eye keeps it out of walls in corners
	VR_HM_RoomToWorld( ox, oz, yaw, scale, eyeW );
	eyeW[2] = ofsY;
	VectorAdd( viewOrg, eyeW, end );
	{
		static const vec3_t emins = { -2, -2, -2 }, emaxs = { 2, 2, 2 };
		vrHmTrace_t tr;

		VR_HM_Trace( &tr, viewOrg, end, emins, emaxs, ps->clientNum, CONTENTS_SOLID | CONTENTS_PLAYERCLIP );
		if ( tr.fraction < 1.0f && !tr.startsolid ) {
			ox *= tr.fraction;
			oz *= tr.fraction;
			eyeW[0] *= tr.fraction;
			eyeW[1] *= tr.fraction;
		}
	}

	originOut[0] = head[0] - ox;
	originOut[1] = head[1] - ofsY / scale;
	originOut[2] = head[2] - oz;
	*fadeOut = fade;

	VectorCopy( eyeW, vhh.eyeOfs );
	vhh.fade = fade;
	vhh.reason = reason;
	if ( VR_HM_DebugLevel() >= 1 && Sys_Milliseconds() - vhh.lastPrint >= ( VR_HM_DebugLevel() >= 2 ? 100 : 500 ) ) {
		vhh.lastPrint = Sys_Milliseconds();
		Com_Printf( "vrhm head: t %d off %.3f %.3f %.3f m eye %+.2f %+.2f %+.2f (h %.2f) %s debt %.2f fade %.2f duck %.1f "
			"view %.1f %.1f %.1f snap %.1f %.1f %.1f vh %d ground %d\n",
			Sys_Milliseconds(), head[0] - anchor[0], head[1] - anchor[1], head[2] - anchor[2],
			eyeW[0], eyeW[1], eyeW[2], sqrtf( eyeW[0] * eyeW[0] + eyeW[1] * eyeW[1] ), vr_hhReason[reason], vhh.debt,
			fade, vhh.duck, viewOrg[0], viewOrg[1], viewOrg[2], ps->origin[0], ps->origin[1], ps->origin[2],
			ps->viewheight, ps->groundEntityNum );
	}
}

void VR_HM_HeadCmd( usercmd_t *cmd, float bodyYaw ) {
	const float a = DEG2RAD( bodyYaw ), c = cosf( a ), s = sinf( a );
	float f, l, k, m;
	int fm, rm;

	if ( !vhh.registered || ( vhh.debtWorld[0] == 0.0f && vhh.debtWorld[1] == 0.0f ) ) {
		return;
	}
	if ( cmd->forwardmove || cmd->rightmove ) {
		return;		// the stick or the keys move the body
	}
	// the debt in the body's frame, as a speed of about 8 per second of it
	f = c * vhh.debtWorld[0] + s * vhh.debtWorld[1];
	l = -s * vhh.debtWorld[0] + c * vhh.debtWorld[1];
	k = VR_HM_STEP_GAIN * 127.0f / VR_HM_SPEED;
	f *= k;
	l *= k;
	m = fabsf( f ) > fabsf( l ) ? fabsf( f ) : fabsf( l );
	if ( m > VR_HM_STEP_MAX ) {
		f *= VR_HM_STEP_MAX / m;
		l *= VR_HM_STEP_MAX / m;
	}
	fm = (int)floorf( f + 0.5f );
	rm = (int)floorf( -l + 0.5f );
	if ( !fm && !rm ) {
		// less than one unit of command: one, so the last bit of the step isn't lost
		if ( fabsf( f ) >= fabsf( l ) ) {
			fm = f > 0.0f ? 1 : -1;
		} else {
			rm = l > 0.0f ? -1 : 1;
		}
	}
	cmd->forwardmove = ClampChar( fm );
	cmd->rightmove = ClampChar( rm );
	cmd->buttons |= BUTTON_WALKING;		// slow and silent, as a walking player
}
