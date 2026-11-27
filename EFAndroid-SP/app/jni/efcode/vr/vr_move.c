/*
===========================================================================
AwayTeamEF VR: head position, room-scale movement, analog movement and crouch (EF_VR only).

This file is part of AwayTeamEF and is licensed under the GNU General
Public License, version 2 or (at your option) any later version.
===========================================================================

Compiled into the engine target (an INTERFACE source of ef_vr). The design is in
docs/vr/MOVEMENT.md; in short:

- The head origin (vr.h) is the point of the room over the player's origin, at the standing
  eye height. The eyes are drawn at the body's eye plus (head - head origin), so leaning,
  crouching and stepping move the view at vr_worldScale units per metre.
- Within vr_leanRadius of the head origin, the head is a view offset only: a lean. That is
  smaller than the player's box, so a lean can't put the eye into a wall.
- Past it, single player moves the body after the head through the game's collision (the SP
  bridge traces the player's box before each ClientThink), and the head origin moves by what
  the move achieved. A wall stops the body, so the head ends up outside the box: the eye is
  pushed back out of the wall and the view fades to black as it goes deeper.
- Without the SP bridge (Holomatch, a co-op guest) the body doesn't follow: the head origin
  is dragged along so the head stays within vr_leanRadius.
- Crouch: the real head going vr_crouchThreshold below the recentred height crouches the game
  (vr_seated 1 turns that off). The crouch button lowers the view by the game's own crouch drop
  and crouches the game too. The cgame keeps the eye at the standing height in VR, so the
  view height comes only from here and the two never add up.
- The stick gives speed in proportion to its deflection (forwardmove/rightmove), relative to
  the head or the off-hand controller (vr_moveDirection).
- Turning (vr_input_engine.c) changes the body yaw. The head origin is turned about the head
  at the same time, so a snap turn pivots on the player's head, not on the body's origin.
*/

#include "../client/client.h"
#include "vr.h"
#include "vr_input.h"
#include "vr_move.h"

extern kbutton_t	in_speed;			// cl_input.c
extern int			SP_IsActive( void );	// sp_bridge.cpp: a single-player game is running

#define VM_HISTORY			16
#define VM_SP_BUTTON_USE	32		// the SP game's BUTTON_USE ("+use", button 5)
#define VM_CROUCH_DROP		16.0f	// the SP game's eye drop from standing to crouched, units
#define VM_CROUCH_TIME		0.1f	// seconds to ease the button crouch (the cgame's DUCK_TIME)
#define VM_MAX_STEP			24.0f	// longest room-scale move in one game tick, units
#define VM_FADE_START		0.03f	// metres into a wall before the view starts to fade
#define VM_FADE_RANGE		0.12f	// ... and how much deeper it is black
#define VM_SP_TIMEOUT		1500	// ms without VR_Move_SPView: the SP bridge isn't driving the body

typedef struct {
	int		time;			// level time of the game tick
	float	anchor[3];		// the head origin after that tick's room-scale move
	vec3_t	origin;			// the player's origin in that tick's snapshot
	int		viewHeight;
} vmRecord_t;

static struct {
	qboolean	registered;
	cvar_t		*roomScale;
	cvar_t		*leanRadius;
	cvar_t		*seated;
	cvar_t		*crouchThreshold;
	cvar_t		*moveDirection;
	cvar_t		*vignette;
	cvar_t		*debugHead;
	cvar_t		*debugStick;
	cvar_t		*debugMove;
	cvar_t		*worldScale;
	cvar_t		*deadzone;

	qboolean	valid;			// a tracked head since the last recentre
	unsigned	recenterSeen;
	float		head[3];		// the head (between the eyes), reference space, plus vr_debugHead
	float		headYaw;		// radians, counter-clockwise from the room's forward (-Z)
	float		offhandYaw;
	float		anchor[3];		// the head origin, before the button crouch and the wall push-back
	float		want[2];		// x, z: how far the body should still move after the head
	float		wantYaw;		// the body yaw of the request VR_Move_SPWant handed out
	float		pivot;			// degrees of turn whose pivot about the head is still due

	qboolean	gameInput;		// the stick and buttons go to the game (no menu)
	float		stick[2];		// the move stick after the dead zone: x right, y forward
	float		stickMag;
	qboolean	crouchButton;
	float		crouchBlend;	// 0..1 of the button crouch's view drop
	qboolean	headCrouch;
	float		turning;		// 0..1 of full smooth-turn speed
	float		fade;
	vec3_t		eyeWorld;		// vr_debugMove: where the eye ended up, after the wall check

	int			spViewTime;		// when VR_Move_SPView last ran
	vmRecord_t	hist[VM_HISTORY];
	int			histCount;
	int			histNext;

	int			lastPrint;
} vm;

void VR_Move_Turned( float deltaYaw, float amount );

// vr_debugTurn <degrees>: a snap turn without the stick (positive = left)
static void VM_DebugTurn_f( void ) {
	const float deg = (float)atof( Cmd_Argv( 1 ) );

	cl.viewangles[YAW] += deg;
	VR_Move_Turned( deg, 0.0f );
}

static void VM_Register( void ) {
	vm.roomScale = Cvar_Get( "vr_roomScale", "1", CVAR_ARCHIVE );
	Cvar_CheckRange( vm.roomScale, 0, 1, qtrue );
	Cvar_SetDescription( vm.roomScale, "VR: stepping in the room moves the player, through the game's collision (single player). 0: the head only moves the view, within vr_leanRadius." );
	vm.leanRadius = Cvar_Get( "vr_leanRadius", "0.3", CVAR_ARCHIVE );
	Cvar_CheckRange( vm.leanRadius, 0.05f, 0.45f, qfalse );
	Cvar_SetDescription( vm.leanRadius, "VR: metres the head may move from the body before the body follows it (a lean)." );
	vm.seated = Cvar_Get( "vr_seated", "0", CVAR_ARCHIVE );
	Cvar_CheckRange( vm.seated, 0, 1, qtrue );
	Cvar_SetDescription( vm.seated, "VR: 1 seated play (crouch with the button only), 0 standing (a real crouch crouches the player)." );
	vm.crouchThreshold = Cvar_Get( "vr_crouchThreshold", "0.3", CVAR_ARCHIVE );
	Cvar_CheckRange( vm.crouchThreshold, 0, 1, qfalse );
	Cvar_SetDescription( vm.crouchThreshold, "VR: metres the head must go below the recentred height to crouch the player (0: off)." );
	vm.moveDirection = Cvar_Get( "vr_moveDirection", "0", CVAR_ARCHIVE );
	Cvar_CheckRange( vm.moveDirection, 0, 1, qtrue );
	Cvar_SetDescription( vm.moveDirection, "VR: the move stick goes where 0 the head or 1 the off-hand controller points." );
	vm.vignette = Cvar_Get( "vr_comfortVignette", "0", CVAR_ARCHIVE );
	Cvar_CheckRange( vm.vignette, 0, 1, qfalse );
	Cvar_SetDescription( vm.vignette, "VR: darken the edges of the view while moving with the stick or turning smoothly (0 off, up to 1 strong)." );
	vm.debugHead = Cvar_Get( "vr_debugHead", "", 0 );
	Cvar_SetDescription( vm.debugHead, "VR debugging: pretend head movement, \"side forward up\" in metres." );
	vm.debugStick = Cvar_Get( "vr_debugStick", "", 0 );
	Cvar_SetDescription( vm.debugStick, "VR debugging: pretend move-stick position, \"x y\" (-1..1), in play." );
	vm.debugMove = Cvar_Get( "vr_debugMove", "0", 0 );
	Cvar_SetDescription( vm.debugMove, "VR debugging: print the head, body and movement state twice a second." );
	vm.worldScale = Cvar_Get( "vr_worldScale", "32", 0 );
	vm.deadzone = Cvar_Get( "vr_stickDeadzone", "0.25", CVAR_ARCHIVE );
	Cmd_AddCommand( "vr_debugTurn", VM_DebugTurn_f );
	vm.registered = qtrue;
}

static float VM_Scale( void ) {
	float s = vm.worldScale->value;
	return s < 1.0f ? 1.0f : s;
}

// "a b c" -> up to n floats; missing ones are 0. Returns how many were read.
static int VM_ParseFloats( const cvar_t *cv, float *out, int n ) {
	float v[3] = { 0.0f, 0.0f, 0.0f };
	int i, got = 0;

	if ( cv->string[0] ) {
		got = sscanf( cv->string, "%f %f %f", &v[0], &v[1], &v[2] );
	}
	for ( i = 0; i < n && i < 3; i++ ) {
		out[i] = v[i];
	}
	return got < 0 ? 0 : got > n ? n : got;
}

// the yaw of a pose's forward (-Z) axis, radians, counter-clockwise seen from above (to the left)
static float VM_Yaw( const float q[4] ) {
	const float fx = -2.0f * ( q[0] * q[2] + q[3] * q[1] );
	const float fz = -( 1.0f - 2.0f * ( q[0] * q[0] + q[1] * q[1] ) );

	if ( fabsf( fx ) + fabsf( fz ) < 1e-4f ) {
		return 0.0f;	// looking straight up or down
	}
	return atan2f( -fx, -fz );
}

// reference space (x, z) -> the game's body frame (forward, left) and back
static void VM_RoomToBody( float x, float z, float *fwd, float *left ) {
	*fwd = -z;
	*left = -x;
}

static void VM_BodyToRoom( float fwd, float left, float *x, float *z ) {
	*x = -left;
	*z = -fwd;
}

// a recentre (vr_recenter, the runtime's own): the head origin is the head again
static void VM_CheckRecenter( void ) {
	const unsigned count = VR_GetRecenterCount();

	if ( count != vm.recenterSeen ) {
		vm.recenterSeen = count;
		vm.valid = qfalse;		// re-anchored at the next tracked head
	}
}

static void VM_Reanchor( void ) {
	VectorCopy( vm.head, vm.anchor );
	vm.want[0] = vm.want[1] = 0.0f;
	vm.pivot = 0.0f;
	vm.histCount = 0;
	vm.valid = qtrue;
}

// the head origin the renderer gets when nothing pushes it: the anchor, lowered by the button crouch
static void VM_EyeOrigin( const float anchor[3], float out[3] ) {
	out[0] = anchor[0];
	out[1] = anchor[1] + vm.crouchBlend * VM_CROUCH_DROP / VM_Scale();
	out[2] = anchor[2];
}

// turn the head origin by deg (counter-clockwise) about the head: the body yaw turns by the
// same amount, so the eye stays where it was in the world
static void VM_PivotAnchor( float anchor[3], float deg ) {
	const float a = DEG2RAD( deg ), c = cosf( a ), s = sinf( a );
	float f, l, f2, l2, x, z;

	VM_RoomToBody( vm.head[0] - anchor[0], vm.head[2] - anchor[2], &f, &l );
	f2 = f * c + l * s;
	l2 = -f * s + l * c;
	VM_BodyToRoom( f2, l2, &x, &z );
	anchor[0] = vm.head[0] - x;
	anchor[2] = vm.head[2] - z;
}

void VR_Move_Turned( float deltaYaw, float amount ) {
	if ( !vm.registered ) {
		return;
	}
	vm.pivot += deltaYaw;	// applied with the usercmd that carries the turn (VR_Move_Cmd)
	vm.turning = amount;
}

void VR_Move_CrouchButton( int down ) {
	vm.crouchButton = down ? qtrue : qfalse;
}

void VR_Move_Frame( const void *inState, int game, float dt ) {
	const vrInputState_t *in = (const vrInputState_t *)inState;
	float dbg[3], xr[3], raw[2], mag, ox, oz, d, r;
	qboolean spDriven;

	if ( !vm.registered ) {
		VM_Register();
	}
	vm.gameInput = ( game && in->active ) ? qtrue : qfalse;

	// vr_debugHead "side forward up" -> reference-space axes, added to the eyes too
	VM_ParseFloats( vm.debugHead, dbg, 3 );
	xr[0] = dbg[0];
	xr[1] = dbg[2];
	xr[2] = -dbg[1];
	VR_SetDebugHeadOffset( xr );

	VM_CheckRecenter();
	if ( in->active && in->head.valid ) {
		vm.head[0] = in->head.position[0] + xr[0];
		vm.head[1] = in->head.position[1] + xr[1];
		vm.head[2] = in->head.position[2] + xr[2];
		vm.headYaw = VM_Yaw( in->head.orientation );
		vm.offhandYaw = in->aim[VR_HAND_LEFT].valid ? VM_Yaw( in->aim[VR_HAND_LEFT].orientation ) : vm.headYaw;
		if ( !vm.valid ) {
			VM_Reanchor();
		}
	}

	// the move stick: a round dead zone, then the rest of the throw scales the speed
	raw[0] = in->move[0];
	raw[1] = in->move[1];
	if ( VM_ParseFloats( vm.debugStick, dbg, 2 ) ) {
		raw[0] = dbg[0];
		raw[1] = dbg[1];
	}
	mag = sqrtf( raw[0] * raw[0] + raw[1] * raw[1] );
	if ( !vm.gameInput || mag <= vm.deadzone->value ) {
		vm.stick[0] = vm.stick[1] = vm.stickMag = 0.0f;
	} else {
		float m = ( mag - vm.deadzone->value ) / ( 1.0f - vm.deadzone->value );
		if ( m > 1.0f ) {
			m = 1.0f;
		}
		vm.stick[0] = raw[0] / mag * m;
		vm.stick[1] = raw[1] / mag * m;
		vm.stickMag = m;
	}

	// the button crouch eases the view down over the game's own duck time
	{
		const float target = ( vm.crouchButton && vm.gameInput ) ? 1.0f : 0.0f;
		const float step = dt / VM_CROUCH_TIME;
		if ( vm.crouchBlend < target ) {
			vm.crouchBlend = vm.crouchBlend + step > target ? target : vm.crouchBlend + step;
		} else if ( vm.crouchBlend > target ) {
			vm.crouchBlend = vm.crouchBlend - step < target ? target : vm.crouchBlend - step;
		}
	}

	if ( !vm.valid ) {
		VR_SetComfort( 0.0f, 0.0f );
		return;
	}

	// a real crouch, with 5 cm of hysteresis
	if ( !vm.seated->integer && vm.crouchThreshold->value > 0.0f ) {
		const float dy = vm.head[1] - vm.anchor[1];
		if ( vm.headCrouch ) {
			vm.headCrouch = dy < -( vm.crouchThreshold->value - 0.05f ) ? qtrue : qfalse;
		} else {
			vm.headCrouch = dy < -vm.crouchThreshold->value ? qtrue : qfalse;
		}
	} else {
		vm.headCrouch = qfalse;
	}

	// room-scale: past the lean radius, the body follows the head
	ox = vm.head[0] - vm.anchor[0];
	oz = vm.head[2] - vm.anchor[2];
	d = sqrtf( ox * ox + oz * oz );
	r = vm.leanRadius->value;
	if ( r > 15.0f / VM_Scale() ) {
		r = 15.0f / VM_Scale();	// stay inside the player's box (16 units each side)
	}
	spDriven = ( Sys_Milliseconds() - vm.spViewTime ) < VM_SP_TIMEOUT ? qtrue : qfalse;
	vm.want[0] = vm.want[1] = 0.0f;
	if ( d > r ) {
		const float k = 1.0f - r / d;
		if ( spDriven && vm.roomScale->integer ) {
			vm.want[0] = ox * k;	// the SP bridge moves the body, then the head origin follows
			vm.want[1] = oz * k;
		} else {
			vm.anchor[0] += ox * k;	// no body to move: keep the head within the radius
			vm.anchor[2] += oz * k;
		}
	}
	if ( !spDriven ) {
		float origin[3];
		VM_EyeOrigin( vm.anchor, origin );
		VR_SetHeadOrigin( origin );
		vm.fade = 0.0f;
	}

	{
		float moving = vm.stickMag > vm.turning ? vm.stickMag : vm.turning;
		VR_SetComfort( vm.fade, vm.vignette->value * moving );
	}
	vm.turning = 0.0f;

	if ( vm.debugMove->integer && Sys_Milliseconds() - vm.lastPrint > ( vm.debugMove->integer > 1 ? 100 : 500 ) ) {
		const vmRecord_t *last = vm.histCount ? &vm.hist[( vm.histNext + VM_HISTORY - 1 ) % VM_HISTORY] : NULL;
		vm.lastPrint = Sys_Milliseconds();
		Com_Printf( "vrmove: t %d head off %+.3f %+.3f %+.3f m, want %+.3f %+.3f, stick %.2f, crouch head %d button %d, fade %.2f, %s,"
			" body %.1f %.1f %.1f vh %d yaw %.1f eye %.1f %.1f %.1f\n", Sys_Milliseconds(),
			vm.head[0] - vm.anchor[0], vm.head[1] - vm.anchor[1], vm.head[2] - vm.anchor[2],
			vm.want[0], vm.want[1], vm.stickMag, vm.headCrouch, vm.crouchButton, vm.fade,
			spDriven ? "sp" : "drag", last ? last->origin[0] : 0.0f, last ? last->origin[1] : 0.0f,
			last ? last->origin[2] : 0.0f, last ? last->viewHeight : 0, cl.viewangles[YAW],
			vm.eyeWorld[0], vm.eyeWorld[1], vm.eyeWorld[2] );
	}
}

void VR_Move_Cmd( usercmd_t *cmd ) {
	int maxMove;
	qboolean running;

	if ( !vm.registered || !vm.gameInput ) {
		return;
	}

	// a turn made last frame goes to the game in this command: pivot the head origin with it
	if ( vm.pivot != 0.0f && vm.valid ) {
		int i;
		VM_PivotAnchor( vm.anchor, vm.pivot );
		for ( i = 0; i < vm.histCount; i++ ) {
			VM_PivotAnchor( vm.hist[i].anchor, vm.pivot );
		}
		vm.pivot = 0.0f;
	}

	// proportional speed: full deflection is the run (or walk) speed of the keyboard
	running = ( in_speed.active ^ ( cl_run->integer != 0 ) ) ? qtrue : qfalse;
	maxMove = running ? 127 : 64;
	if ( vm.stickMag > 0.0f ) {
		const float yaw = vm.moveDirection->integer == 1 ? vm.offhandYaw : vm.headYaw;
		const float c = cosf( yaw ), s = sinf( yaw );
		const float x = vm.stick[0], y = vm.stick[1];
		const float fwd = y * c + x * s;
		const float right = x * c - y * s;

		cmd->forwardmove = ClampChar( cmd->forwardmove + (int)floorf( fwd * maxMove + 0.5f ) );
		cmd->rightmove = ClampChar( cmd->rightmove + (int)floorf( right * maxMove + 0.5f ) );
		if ( vm.stickMag * maxMove < 64.0f ) {
			cmd->buttons |= BUTTON_WALKING;	// slow: the walk animations, no footsteps
		}
	}

	// crouch (a jump wins)
	if ( ( vm.headCrouch || vm.crouchButton ) && cmd->upmove <= 0 ) {
		cmd->upmove = -127;
	}

	// The SP game leans while Use is held with a strafe and no forward move. In VR the head is
	// the lean, so a tiny forward move keeps that from adding a second one.
	if ( SP_IsActive() && ( cmd->buttons & VM_SP_BUTTON_USE ) && cmd->rightmove && !cmd->forwardmove &&
		cmd->upmove <= 0 ) {
		cmd->forwardmove = 1;
	}
}

// ---------------------------------------------------------------------------
// single player: the SP bridge moves the body after the head
// ---------------------------------------------------------------------------

int VR_Move_SPWant( float bodyYaw, float want[3] ) {
	const float a = DEG2RAD( bodyYaw ), scale = vm.registered ? VM_Scale() : 32.0f;
	float f, l, len;

	if ( !vm.registered || !vm.valid || ( vm.want[0] == 0.0f && vm.want[1] == 0.0f ) ) {
		return 0;
	}
	VM_RoomToBody( vm.want[0], vm.want[1], &f, &l );
	want[0] = scale * ( f * cosf( a ) - l * sinf( a ) );
	want[1] = scale * ( f * sinf( a ) + l * cosf( a ) );
	want[2] = 0.0f;
	len = sqrtf( want[0] * want[0] + want[1] * want[1] );
	if ( len > VM_MAX_STEP ) {
		want[0] *= VM_MAX_STEP / len;	// the rest goes in the next ticks
		want[1] *= VM_MAX_STEP / len;
	}
	vm.wantYaw = bodyYaw;
	return 1;
}

void VR_Move_SPMoved( const float want[3], const float got[3], int canMove ) {
	const float a = DEG2RAD( vm.wantYaw ), scale = VM_Scale();
	const float *move = canMove ? got : want;
	float f, l, x, z;

	vm.want[0] = vm.want[1] = 0.0f;
	if ( !vm.registered || !vm.valid ) {
		return;
	}
	// canMove 0: the game holds the player; the head origin follows the head all the same,
	// so no move builds up behind a cutscene
	f = move[0] * cosf( a ) + move[1] * sinf( a );
	l = -move[0] * sinf( a ) + move[1] * cosf( a );
	VM_BodyToRoom( f / scale, l / scale, &x, &z );
	vm.anchor[0] += x;
	vm.anchor[2] += z;
}

void VR_Move_SPRecord( int levelTime, const float origin[3], int viewHeight ) {
	vmRecord_t *rec;

	if ( !vm.registered || !vm.valid ) {
		return;
	}
	if ( vm.histCount && vm.hist[( vm.histNext + VM_HISTORY - 1 ) % VM_HISTORY].time >= levelTime ) {
		vm.histCount = 0;	// time went back (a load, a restart): start again
	}
	rec = &vm.hist[vm.histNext];
	rec->time = levelTime;
	VectorCopy( vm.anchor, rec->anchor );
	VectorCopy( origin, rec->origin );
	rec->viewHeight = viewHeight;
	vm.histNext = ( vm.histNext + 1 ) % VM_HISTORY;
	if ( vm.histCount < VM_HISTORY ) {
		vm.histCount++;
	}
}

void VR_Move_SPView( int cgTime, float viewYaw, int standViewHeight, vrMoveTrace_t trace ) {
	const float scale = vm.registered ? VM_Scale() : 32.0f;
	const vmRecord_t *a = NULL, *b = NULL;
	float anchor[3], origin[3], eyeOrigin[3], frac = 0.0f;
	int i, viewHeight = standViewHeight;

	vm.spViewTime = Sys_Milliseconds();
	if ( !vm.registered ) {
		return;
	}
	VM_CheckRecenter();
	if ( !vm.valid ) {
		return;
	}

	// the head origin that goes with the body the cgame draws at cgTime: it interpolates the
	// player's origin between the two snapshots around cgTime, so do the same with the anchor
	for ( i = 0; i < vm.histCount; i++ ) {
		const vmRecord_t *r = &vm.hist[( vm.histNext + VM_HISTORY - vm.histCount + i ) % VM_HISTORY];
		if ( r->time <= cgTime ) {
			a = r;
		} else if ( !b ) {
			b = r;
		}
	}
	if ( a && b && b->time > a->time ) {
		frac = (float)( cgTime - a->time ) / (float)( b->time - a->time );
	} else if ( !a ) {
		a = b;
		b = NULL;
	}
	if ( a ) {
		for ( i = 0; i < 3; i++ ) {
			anchor[i] = b ? a->anchor[i] + frac * ( b->anchor[i] - a->anchor[i] ) : a->anchor[i];
			origin[i] = b ? a->origin[i] + frac * ( b->origin[i] - a->origin[i] ) : a->origin[i];
		}
		anchor[1] = vm.anchor[1];
		viewHeight = b ? b->viewHeight : a->viewHeight;
	} else {
		VectorCopy( vm.anchor, anchor );
	}
	VM_EyeOrigin( anchor, eyeOrigin );

	// the head in a wall: push the eye back to where the trace from the body's eye stops, and fade
	vm.fade = 0.0f;
	if ( a && trace ) {
		const float yaw = DEG2RAD( viewYaw ), c = cosf( yaw ), s = sinf( yaw );
		float f, l, u, start[3], end[3], endpos[3];

		VM_RoomToBody( vm.head[0] - eyeOrigin[0], vm.head[2] - eyeOrigin[2], &f, &l );
		u = vm.head[1] - eyeOrigin[1];
		end[0] = origin[0] + scale * ( f * c - l * s );
		end[1] = origin[1] + scale * ( f * s + l * c );
		end[2] = origin[2] + standViewHeight + scale * u;
		start[0] = origin[0];
		start[1] = origin[1];
		start[2] = origin[2] + viewHeight;
		VectorCopy( end, vm.eyeWorld );
		if ( trace( start, end, endpos ) < 1.0f ) {
			const float dx = end[0] - endpos[0], dy = end[1] - endpos[1], dz = end[2] - endpos[2];
			const float depth = sqrtf( dx * dx + dy * dy + dz * dz ) / scale;
			float x, z;

			VM_BodyToRoom( ( dx * c + dy * s ) / scale, ( -dx * s + dy * c ) / scale, &x, &z );
			eyeOrigin[0] += x;
			eyeOrigin[1] += dz / scale;
			eyeOrigin[2] += z;
			vm.fade = ( depth - VM_FADE_START ) / VM_FADE_RANGE;
			vm.fade = vm.fade < 0.0f ? 0.0f : vm.fade > 1.0f ? 1.0f : vm.fade;
			VectorCopy( endpos, vm.eyeWorld );
		}
	}
	VR_SetHeadOrigin( eyeOrigin );
}
