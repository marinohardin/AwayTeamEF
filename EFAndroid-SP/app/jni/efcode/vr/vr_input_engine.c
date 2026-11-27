/*
===========================================================================
AwayTeamEF VR: the controller map, from vr_input.c's state to the engine's input (EF_VR only).

This file is part of AwayTeamEF and is licensed under the GNU General
Public License, version 2 or (at your option) any later version.
===========================================================================

Compiled into the engine target (an INTERFACE source of ef_vr), with the engine's own
defines and headers. Everything goes through paths the engine already has, so the game
modules need no change:

  in play (no key catcher)
    left stick           move, at a speed in proportion to the deflection, relative to the
                         head or the off-hand (vr_move.c: VR_Move_Cmd in CL_CreateCmd)
    right stick x        turn: snap (vr_turnMode 0, vr_snapTurnAngle degrees) or smooth
                         (vr_turnMode 1, vr_turnSpeed degrees per second), on cl.viewangles
    right trigger        +attack
    left trigger         +altattack
    A / B / X            +moveup (jump) / crouch (vr_move.c) / +use
    right stick click    weapnext when tapped; held VR_WHEEL_HOLD ms: the weapon wheel (vr_wheel.c)
                         until released, which picks; fire and turning wait while it's open
    left stick click     weapprev when tapped; held VR_RECENTER_HOLD ms: recentre the head
    Y, left menu button  Escape (the in-game menu)
  in play in Holomatch (VR_HM_Active; docs/vr/HOLOMATCH-H1.md §6.1), different from the above:
    right grip           +button2 (use the holdable item)
    left grip, held      +info (the scoreboard)
    Y                    +analysis (the objectives); the left menu button is still Escape
    right stick y        weapnext (up) / weapprev (down)
    right stick click    tapped: the last weapon (weapon <n>); held: the weapon wheel
    left stick click     tapped: walk/run toggle (+speed); held: recentre
    vr_leftHanded swaps the grips with the hands
  in menus and the console (any key catcher)
    right stick          mouse cursor
    right trigger        mouse button 1
    left stick           arrow keys (with key repeat)
    A                    Enter
    B, Y, menu button    Escape

A button held while play and menus swap is let go of in the mode it was pressed in, and it
does nothing in the new mode until it is released.
*/

#include "../client/client.h"
#include "vr.h"
#include "vr_input.h"
#include "vr_move.h"
#include "vr_wheel.h"

#define VR_KEYNUM_BASE		0x7000	// fake key numbers for +commands (kbutton_t tracks two keys)

#define VR_TRIGGER_ON		0.55f
#define VR_TRIGGER_OFF		0.45f
#define VR_SNAP_ON			0.70f	// stick deflection that fires one snap turn
#define VR_SNAP_REARM		0.35f
#define VR_ARROW_ON			0.60f
#define VR_ARROW_DELAY		400		// ms before an arrow key repeats
#define VR_ARROW_REPEAT		120
#define VR_RECENTER_HOLD	700		// ms the left stick is held in for a recentre
#define VR_WHEEL_HOLD		250		// ms the right stick is held in for the weapon wheel

// logical digital inputs
enum {
	IN_ATTACK,
	IN_ALTATTACK,
	IN_JUMP,
	IN_CROUCH,
	IN_USE,
	IN_WEAPNEXT,
	IN_WEAPPREV,
	IN_ITEM,		// Holomatch only
	IN_SCORES,
	IN_ANALYSIS,
	NUM_INPUTS
};

static const struct {
	const char	*gameCmd;		// "+x" holds (with "-x" on release); anything else is sent once
	int			uiKey;			// key event in menus, 0 = nothing
} vr_map[NUM_INPUTS] = {
	{ "+attack",	K_MOUSE1 },
	{ "+altattack",	0 },
	{ "+moveup",	K_ENTER },
	{ "+movedown",	K_ESCAPE },
	{ "+use",		0 },
	{ "weapnext",	0 },
	{ "weapprev",	0 },
	{ "+button2",	0 },
	{ "+info",		0 },
	{ "+analysis",	0 },
};

typedef enum { MODE_NONE, MODE_GAME, MODE_UI } vrMode_t;

static struct {
	qboolean	registered;
	cvar_t		*turnMode;
	cvar_t		*snapTurnAngle;
	cvar_t		*turnSpeed;
	cvar_t		*deadzone;
	cvar_t		*cursorSpeed;
	cvar_t		*leftHanded;
	cvar_t		*debugHand;
	cvar_t		*debugHandPose;
	cvar_t		*debugWheel;

	vrMode_t	mode;
	int			lastTime;

	qboolean	down[NUM_INPUTS];		// logical state, after the trigger thresholds
	vrMode_t	heldIn[NUM_INPUTS];		// where its press went (MODE_NONE = nowhere)
	qboolean	menuDown;				// Escape is held

	qboolean	snapArmed;
	int			prevHeldSince;			// when the left stick click went down (in play), 0 = up
	qboolean	prevRecentered;			// that hold already recentred
	int			nextHeldSince;			// when the right stick click went down (in play), 0 = up
	qboolean	fireBlocked[NUM_INPUTS];	// a trigger held across the wheel: dead until let go
	float		cursorFrac[2];

	int			arrowKey;				// held arrow key, 0 = none
	int			arrowNext;				// time of the next repeat

	qboolean	hm;						// Holomatch H1 is active (its controller map)
	qboolean	weapArmed;				// right stick y back in the middle (Holomatch weapon cycling)
	int			curWeapon, lastWeapon;	// Holomatch: the weapon now and the one before
	qboolean	walkToggle;				// Holomatch: +speed held by the left stick click
} vrc;

int VR_HM_Active( void );	// vr_hm.c

static void VR_RegisterCvars( void ) {
	vrc.turnMode = Cvar_Get( "vr_turnMode", "0", CVAR_ARCHIVE );
	Cvar_CheckRange( vrc.turnMode, 0, 1, qtrue );
	vrc.snapTurnAngle = Cvar_Get( "vr_snapTurnAngle", "45", CVAR_ARCHIVE );
	Cvar_CheckRange( vrc.snapTurnAngle, 5, 180, qfalse );
	vrc.turnSpeed = Cvar_Get( "vr_turnSpeed", "150", CVAR_ARCHIVE );
	Cvar_CheckRange( vrc.turnSpeed, 10, 720, qfalse );
	vrc.deadzone = Cvar_Get( "vr_stickDeadzone", "0.25", CVAR_ARCHIVE );
	Cvar_CheckRange( vrc.deadzone, 0, 0.9f, qfalse );
	vrc.cursorSpeed = Cvar_Get( "vr_menuCursorSpeed", "500", CVAR_ARCHIVE );
	Cvar_CheckRange( vrc.cursorSpeed, 50, 3000, qfalse );
	vrc.leftHanded = Cvar_Get( "vr_leftHanded", "0", CVAR_ARCHIVE );
	Cvar_CheckRange( vrc.leftHanded, 0, 1, qtrue );
	Cvar_SetDescription( vrc.leftHanded, "VR: 1 = the weapon is in the left hand, and the left trigger fires." );
	vrc.debugHand = Cvar_Get( "vr_debugHand", "0", CVAR_TEMP );
	Cvar_CheckRange( vrc.debugHand, 0, 2, qtrue );
	Cvar_SetDescription( vrc.debugHand, "VR debugging: drive the weapon hand without a controller. "
		"1 = a slow scripted roll and sweep, 2 = the fixed pose in vr_debugHandPose." );
	vrc.debugHandPose = Cvar_Get( "vr_debugHandPose", "0.40 0.20 -0.30 0 0 0", CVAR_TEMP );
	Cvar_SetDescription( vrc.debugHandPose, "VR debugging (vr_debugHand 2): the weapon hand as "
		"\"forward right up pitch yaw roll\", metres from the head and degrees from the body's facing." );
	vrc.debugWheel = Cvar_Get( "vr_debugWheel", "0", CVAR_TEMP );
	Cvar_SetDescription( vrc.debugWheel, "VR debugging: 1 = hold the weapon wheel button (the right thumbstick "
		"click), 0 = let go of it." );
	vrc.snapArmed = qtrue;
	vrc.registered = qtrue;
}

// dead zone, then rescaled so the edge of the zone is 0
static float VR_Stick( float v ) {
	float dz = vrc.deadzone->value;
	float a = v < 0 ? -v : v;

	if ( a <= dz ) {
		return 0.0f;
	}
	a = ( a - dz ) / ( 1.0f - dz );
	if ( a > 1.0f ) {
		a = 1.0f;
	}
	return v < 0 ? -a : a;
}

static void VR_Key( int key, qboolean down, int time ) {
	Com_QueueEvent( time, SE_KEY, key, down, 0, NULL );
}

static void VR_GameCmd( int input, qboolean down, int time ) {
	const char *cmd = vr_map[input].gameCmd;

	if ( input == IN_CROUCH ) {
		VR_Move_CrouchButton( down );	// the view goes down with the game's crouch
		return;
	}
	if ( input == IN_WEAPPREV ) {
		// a tap is weapprev (on release; Holomatch: the walk/run toggle), a long hold recentres
		// (VR_EngineInput)
		if ( down ) {
			vrc.prevHeldSince = time ? time : 1;
			vrc.prevRecentered = qfalse;
		} else {
			if ( vrc.prevHeldSince && !vrc.prevRecentered ) {
				if ( vrc.hm ) {
					vrc.walkToggle = !vrc.walkToggle;
					Cbuf_AddText( va( "%cspeed %d %d\n", vrc.walkToggle ? '+' : '-', VR_KEYNUM_BASE + NUM_INPUTS, time ) );
				} else {
					Cbuf_AddText( "weapprev\n" );
				}
			}
			vrc.prevHeldSince = 0;
		}
		return;
	}
	if ( input == IN_WEAPNEXT ) {
		// a tap (on release) is weapnext, Holomatch: the last weapon; a hold opens the weapon
		// wheel (VR_EngineInput) and letting go picks
		if ( down ) {
			vrc.nextHeldSince = time ? time : 1;
		} else {
			if ( VR_Wheel_IsOpen() ) {
				VR_Wheel_Close( qtrue, "released" );
			} else if ( vrc.nextHeldSince ) {
				if ( !vrc.hm ) {
					Cbuf_AddText( "weapnext\n" );
				} else if ( vrc.lastWeapon > 0 ) {
					Cbuf_AddText( va( "weapon %d\n", vrc.lastWeapon ) );
				}
			}
			vrc.nextHeldSince = 0;
		}
		return;
	}
	if ( cmd[0] == '+' ) {
		Cbuf_AddText( va( "%c%s %d %d\n", down ? '+' : '-', cmd + 1, VR_KEYNUM_BASE + input, time ) );
	} else if ( down ) {
		Cbuf_AddText( va( "%s\n", cmd ) );
	}
}

static void VR_SetArrow( int key, int time ) {
	if ( key == vrc.arrowKey ) {
		if ( key && time >= vrc.arrowNext ) {
			VR_Key( key, qtrue, time );		// a repeat, like a held keyboard key
			vrc.arrowNext = time + VR_ARROW_REPEAT;
		}
		return;
	}
	if ( vrc.arrowKey ) {
		VR_Key( vrc.arrowKey, qfalse, time );
	}
	vrc.arrowKey = key;
	if ( key ) {
		VR_Key( key, qtrue, time );
		vrc.arrowNext = time + VR_ARROW_DELAY;
	}
}

static void VR_Release( int input, int time ) {
	if ( vrc.heldIn[input] == MODE_GAME ) {
		VR_GameCmd( input, qfalse, time );
	} else if ( vrc.heldIn[input] == MODE_UI && vr_map[input].uiKey ) {
		VR_Key( vr_map[input].uiKey, qfalse, time );
	}
	vrc.heldIn[input] = MODE_NONE;
}

// let go of everything held in the old mode; buttons still down stay dead until released
static void VR_LeaveMode( int time ) {
	int i;

	vrc.prevHeldSince = 0;	// a held left stick click let go by a mode change: neither a tap nor a hold
	vrc.nextHeldSince = 0;	// the same for the right one; an open wheel shuts without a pick
	VR_Wheel_Reset( "left play" );

	for ( i = 0; i < NUM_INPUTS; i++ ) {
		VR_Release( i, time );
	}
	VR_SetArrow( 0, time );
	vrc.cursorFrac[0] = vrc.cursorFrac[1] = 0;
	vrc.snapArmed = qfalse;
}

void VR_EngineInput( const vrInputState_t *in ) {
	qboolean now[NUM_INPUTS];
	vrMode_t mode;
	int time = Sys_Milliseconds();
	float dt, tx;
	int i;

	if ( !vrc.registered ) {
		VR_RegisterCvars();
	}
	dt = vrc.lastTime ? ( time - vrc.lastTime ) * 0.001f : 0.0f;
	if ( dt < 0.0f || dt > 0.1f ) {
		dt = 0.1f;	// after a load or a hitch: no big jump
	}
	vrc.lastTime = time;

	// logical buttons (triggers with hysteresis)
	{	// the weapon hand's trigger fires, the other one is alternate fire
		float fire = vrc.leftHanded->integer ? in->altAttack : in->attack;
		float alt = vrc.leftHanded->integer ? in->attack : in->altAttack;

		now[IN_ATTACK] = fire > ( vrc.down[IN_ATTACK] ? VR_TRIGGER_OFF : VR_TRIGGER_ON );
		now[IN_ALTATTACK] = alt > ( vrc.down[IN_ALTATTACK] ? VR_TRIGGER_OFF : VR_TRIGGER_ON );
	}
	now[IN_JUMP] = ( in->buttons & VR_BUTTON_JUMP ) != 0;
	now[IN_CROUCH] = ( in->buttons & VR_BUTTON_CROUCH ) != 0;
	now[IN_USE] = ( in->buttons & VR_BUTTON_USE ) != 0;
	now[IN_WEAPNEXT] = ( in->buttons & VR_BUTTON_WEAPNEXT ) != 0 || vrc.debugWheel->integer;
	now[IN_WEAPPREV] = ( in->buttons & VR_BUTTON_WEAPPREV ) != 0;

	// Holomatch H1's own controller map (grips, Y, weapon cycling on the right stick)
	vrc.hm = VR_HM_Active() ? qtrue : qfalse;
	{
		const int weaponHand = vrc.leftHanded->integer ? VR_HAND_LEFT : VR_HAND_RIGHT;
		const float item = vrc.hm ? in->squeeze[weaponHand] : 0.0f;
		const float scores = vrc.hm ? in->squeeze[!weaponHand] : 0.0f;

		now[IN_ITEM] = item > ( vrc.down[IN_ITEM] ? VR_TRIGGER_OFF : VR_TRIGGER_ON );
		now[IN_SCORES] = scores > ( vrc.down[IN_SCORES] ? VR_TRIGGER_OFF : VR_TRIGGER_ON );
		now[IN_ANALYSIS] = vrc.hm && ( in->buttons & VR_BUTTON_Y ) != 0;
	}
	if ( vrc.hm ) {
		const int w = cl.snap.ps.weapon;
		if ( w != vrc.curWeapon ) {
			if ( vrc.curWeapon > 0 ) {
				vrc.lastWeapon = vrc.curWeapon;
			}
			vrc.curWeapon = w;
		}
	} else if ( vrc.walkToggle ) {
		vrc.walkToggle = qfalse;
		Cbuf_AddText( va( "-speed %d %d\n", VR_KEYNUM_BASE + NUM_INPUTS, time ) );
	}

	// Escape works the same everywhere: it opens the in-game menu and backs out of menus (Y too,
	// outside Holomatch)
	if ( ( ( in->buttons & VR_BUTTON_MENU ) != 0 || ( !vrc.hm && ( in->buttons & VR_BUTTON_Y ) != 0 ) ) != vrc.menuDown ) {
		vrc.menuDown = !vrc.menuDown;
		VR_Key( K_ESCAPE, vrc.menuDown, time );
	}

	if ( !in->active ) {
		mode = MODE_NONE;
	} else if ( Key_GetCatcher() ) {
		mode = MODE_UI;
	} else {
		mode = MODE_GAME;
	}
	if ( mode != vrc.mode ) {
		VR_LeaveMode( time );
		vrc.mode = mode;
	}

	// the weapon wheel: the pointer and the reasons to shut it, before a release picks below.
	// While it's open the triggers do nothing, and one still held when it shuts stays dead
	// until let go: no shot on the release, none from a trigger pulled to pick.
	{
		const float stick[2] = { in->turn[0], in->turn[1] };
		VR_Wheel_Update( stick, mode == MODE_GAME );
	}
	for ( i = IN_ATTACK; i <= IN_ALTATTACK; i++ ) {
		if ( VR_Wheel_IsOpen() ) {
			vrc.fireBlocked[i] = qtrue;
			now[i] = qfalse;
		} else if ( vrc.fireBlocked[i] ) {
			if ( now[i] ) {
				now[i] = qfalse;
			} else {
				vrc.fireBlocked[i] = qfalse;
			}
		}
	}

	for ( i = 0; i < NUM_INPUTS; i++ ) {
		if ( now[i] && !vrc.down[i] ) {
			// a fresh press goes to the current mode
			if ( mode == MODE_GAME ) {
				VR_GameCmd( i, qtrue, time );
				vrc.heldIn[i] = ( vr_map[i].gameCmd[0] == '+' || i == IN_WEAPPREV || i == IN_WEAPNEXT ) ? MODE_GAME : MODE_NONE;
			} else if ( mode == MODE_UI && vr_map[i].uiKey ) {
				VR_Key( vr_map[i].uiKey, qtrue, time );
				vrc.heldIn[i] = MODE_UI;
			}
		} else if ( !now[i] && vrc.down[i] ) {
			VR_Release( i, time );
		}
		vrc.down[i] = now[i];
	}

	tx = VR_Stick( in->turn[0] );

	// head position, room-scale and the move stick (vr_move.c); the stick reaches the game in
	// CL_CreateCmd (VR_Move_Cmd), with a speed in proportion to the deflection
	VR_Move_Frame( in, mode == MODE_GAME, dt );

	if ( mode == MODE_GAME ) {
		float turn = 0.0f;

		if ( vrc.prevHeldSince && !vrc.prevRecentered && time - vrc.prevHeldSince >= VR_RECENTER_HOLD ) {
			VR_Recenter();
			vrc.prevRecentered = qtrue;
			Com_Printf( "VR: recentred\n" );
		}

		if ( vrc.nextHeldSince && !VR_Wheel_IsOpen() && time - vrc.nextHeldSince >= VR_WHEEL_HOLD ) {
			// a hold, never also a tap: if the wheel can't open now the release does nothing
			if ( VR_Wheel_Open() ) {
				for ( i = IN_ATTACK; i <= IN_ALTATTACK; i++ ) {
					if ( vrc.down[i] ) {
						VR_Release( i, time );	// stop firing
						vrc.down[i] = qfalse;
					}
					vrc.fireBlocked[i] = qtrue;
				}
			}
			vrc.nextHeldSince = 0;
		}
		if ( VR_Wheel_IsOpen() ) {
			// the right stick picks on the wheel: no turning, no Holomatch weapon cycling, and
			// a stick still pushed when it shuts must come back to the middle first
			vrc.snapArmed = qfalse;
			vrc.weapArmed = qfalse;
			tx = 0.0f;
		}

		// turning changes the body yaw only; vr_move.c turns the head origin about the head with it
		if ( vrc.turnMode->integer == 1 ) {
			turn = -tx * vrc.turnSpeed->value * dt;
			VR_Move_Turned( turn, tx < 0 ? -tx : tx );
		} else if ( vrc.snapArmed && ( in->turn[0] > VR_SNAP_ON || in->turn[0] < -VR_SNAP_ON ) ) {
			turn = in->turn[0] > 0 ? -vrc.snapTurnAngle->value : vrc.snapTurnAngle->value;
			VR_Move_Turned( turn, 0.0f );
			vrc.snapArmed = qfalse;
		}
		cl.viewangles[YAW] += turn;
		if ( in->turn[0] < VR_SNAP_REARM && in->turn[0] > -VR_SNAP_REARM ) {
			vrc.snapArmed = qtrue;
		}

		// Holomatch: the right stick's y cycles the weapons, once per push
		if ( vrc.hm ) {
			if ( vrc.weapArmed && ( in->turn[1] > VR_SNAP_ON || in->turn[1] < -VR_SNAP_ON ) ) {
				Cbuf_AddText( in->turn[1] > 0 ? "weapnext\n" : "weapprev\n" );
				vrc.weapArmed = qfalse;
			} else if ( in->turn[1] < VR_SNAP_REARM && in->turn[1] > -VR_SNAP_REARM ) {
				vrc.weapArmed = qtrue;
			}
		}
	} else if ( mode == MODE_UI ) {
		float ty = VR_Stick( in->turn[1] );
		int dx, dy, key = 0;

		// right stick: the mouse cursor (stick up = cursor up = negative y)
		vrc.cursorFrac[0] += tx * vrc.cursorSpeed->value * dt;
		vrc.cursorFrac[1] -= ty * vrc.cursorSpeed->value * dt;
		dx = (int)vrc.cursorFrac[0];
		dy = (int)vrc.cursorFrac[1];
		if ( dx || dy ) {
			Com_QueueEvent( time, SE_MOUSE, dx, dy, 0, NULL );
			vrc.cursorFrac[0] -= dx;
			vrc.cursorFrac[1] -= dy;
		}

		// left stick: arrow keys
		if ( in->move[1] > VR_ARROW_ON ) {
			key = K_UPARROW;
		} else if ( in->move[1] < -VR_ARROW_ON ) {
			key = K_DOWNARROW;
		} else if ( in->move[0] > VR_ARROW_ON ) {
			key = K_RIGHTARROW;
		} else if ( in->move[0] < -VR_ARROW_ON ) {
			key = K_LEFTARROW;
		}
		VR_SetArrow( key, time );
	}
}

/*
==================
The hand poses for the game modules (VR_EngineBodyHand, through the single-player bridge's
CG_VR_GETPOSE). Same convention as the eye views (renderervk/tr_vr.c): the offset from the
recentred head origin times vr_worldScale, OpenXR axes turned into the game's, and the body's
yaw left for the game to apply. So a hand drawn at the game's eye plus this offset lines up
with the eyes the renderer draws.
==================
*/

// OpenXR (+X right, +Y up, -Z forward) to the game's body frame (+X forward, +Y left, +Z up)
static void VR_FromXr( const float *xr, float *out ) {
	out[0] = -xr[2];
	out[1] = -xr[0];
	out[2] = xr[1];
}

// vr_debugHand: a pose made up in the body frame, relative to the current head position
static void VR_DebugHand( const vrInputState_t *in, const float *headOrigin, float scale, vrBodyHand_t *out ) {
	float fwd = 0.40f, right = 0.20f, up = -0.30f, rel[3], head[3];
	vec3_t angles, axis[3];
	int i;

	VectorClear( angles );
	if ( vrc.debugHand->integer == 1 ) {
		// slow and easy to follow: a full wrist roll back and forth, a sweep left and right
		float t = Sys_Milliseconds() * 0.001f;
		angles[ROLL] = 90.0f * sin( t * 0.5f );
		angles[YAW] = 35.0f * sin( t * 0.23f );
		angles[PITCH] = 12.0f * sin( t * 0.31f );
	} else {
		sscanf( vrc.debugHandPose->string, "%f %f %f %f %f %f", &fwd, &right, &up,
			&angles[PITCH], &angles[YAW], &angles[ROLL] );
	}
	AnglesToAxis( angles, axis );

	// follow the head: the made-up hand is relative to where the head is now
	head[0] = head[1] = head[2] = 0.0f;
	if ( in->head.valid ) {
		for ( i = 0; i < 3; i++ ) {
			rel[i] = in->head.position[i] - headOrigin[i];
		}
		VR_FromXr( rel, head );
	}
	out->origin[0] = ( head[0] + fwd ) * scale;
	out->origin[1] = ( head[1] - right ) * scale;
	out->origin[2] = ( head[2] + up ) * scale;
	for ( i = 0; i < 3; i++ ) {
		VectorCopy( axis[i], out->axis[i] );
	}
	out->valid = 1;
}

// a rotation (x y z w, OpenXR) as the game's forward, left and up axes
static void VR_QuatToBodyAxes( const float *q, float axis[3][3] ) {
	const float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
	float v[3];

	// the rotation's columns are its right, up and back axes in OpenXR space
	v[0] = -( 2 * ( qx * qz + qw * qy ) ); v[1] = -( 2 * ( qy * qz - qw * qx ) ); v[2] = -( 1 - 2 * ( qx * qx + qy * qy ) );
	VR_FromXr( v, axis[0] );	// forward = -back
	v[0] = -( 1 - 2 * ( qy * qy + qz * qz ) ); v[1] = -( 2 * ( qx * qy + qw * qz ) ); v[2] = -( 2 * ( qx * qz - qw * qy ) );
	VR_FromXr( v, axis[1] );	// left = -right
	v[0] = 2 * ( qx * qy - qw * qz ); v[1] = 1 - 2 * ( qx * qx + qz * qz ); v[2] = 2 * ( qy * qz + qw * qx );
	VR_FromXr( v, axis[2] );	// up
}

int VR_EngineBodyHand( int which, vrBodyHand_t *out ) {
	const vrInputState_t *in = VR_GetInputState();
	const vrPose_t *p;
	float headOrigin[3], rel[3], col[3][3], v[3];
	float qx, qy, qz, qw, scale;
	int hand, i;

	Com_Memset( out, 0, sizeof( *out ) );
	if ( !vrc.registered ) {
		VR_RegisterCvars();
	}
	if ( !in || !Cvar_VariableIntegerValue( "vr_active" ) ) {
		return 0;
	}
	scale = Cvar_VariableValue( "vr_worldScale" );
	if ( scale <= 0.0f ) {
		scale = 32.0f;
	}
	VR_GetHeadOrigin( headOrigin );

	if ( which == VR_BODY_WEAPON && vrc.debugHand->integer ) {
		VR_DebugHand( in, headOrigin, scale, out );
		return 1;
	}
	if ( which == VR_BODY_HEAD ) {
		// the eyes as the renderer draws them this frame (vr_debugHead included), else the head
		float eye0[3], eye1[3];
		vrEyeView_t view;

		VR_GetEyeOffset( 0, eye0 );
		VR_GetEyeOffset( 1, eye1 );
		if ( eye0[0] != eye1[0] || eye0[1] != eye1[1] || eye0[2] != eye1[2] ) {
			for ( i = 0; i < 3; i++ ) {
				rel[i] = 0.5f * ( eye0[i] + eye1[i] );
			}
			VR_FromXr( rel, out->origin );
			VectorScale( out->origin, scale, out->origin );
			// the eyes' orientation (both eyes share it): effects face the real head with it
			if ( VR_GetEyeView( 0, &view ) ) {
				VR_QuatToBodyAxes( view.orientation, out->axis );
			} else {
				out->axis[0][0] = out->axis[1][1] = out->axis[2][2] = 1.0f;
			}
			out->valid = 1;
			return 1;
		}
		if ( !in->head.valid ) {
			return 0;
		}
		p = &in->head;
	} else {
	if ( !in->active ) {
		return 0;
	}
	hand = ( which == VR_BODY_WEAPON ) != ( vrc.leftHanded->integer != 0 ) ? VR_HAND_RIGHT : VR_HAND_LEFT;
	p = &in->aim[hand];
	}
	if ( !p->valid ) {
		return 0;
	}

	for ( i = 0; i < 3; i++ ) {
		rel[i] = p->position[i] - headOrigin[i];
	}
	VR_FromXr( rel, out->origin );
	VectorScale( out->origin, scale, out->origin );

	// the rotation's columns: the controller's right, up and back axes in OpenXR space
	qx = p->orientation[0]; qy = p->orientation[1]; qz = p->orientation[2]; qw = p->orientation[3];
	col[0][0] = 1 - 2 * ( qy * qy + qz * qz ); col[0][1] = 2 * ( qx * qy + qw * qz ); col[0][2] = 2 * ( qx * qz - qw * qy );
	col[1][0] = 2 * ( qx * qy - qw * qz ); col[1][1] = 1 - 2 * ( qx * qx + qz * qz ); col[1][2] = 2 * ( qy * qz + qw * qx );
	col[2][0] = 2 * ( qx * qz + qw * qy ); col[2][1] = 2 * ( qy * qz - qw * qx ); col[2][2] = 1 - 2 * ( qx * qx + qy * qy );

	v[0] = -col[2][0]; v[1] = -col[2][1]; v[2] = -col[2][2];	// forward = -back
	VR_FromXr( v, out->axis[0] );
	v[0] = -col[0][0]; v[1] = -col[0][1]; v[2] = -col[0][2];	// left = -right
	VR_FromXr( v, out->axis[1] );
	VR_FromXr( col[1], out->axis[2] );
	out->valid = 1;
	return 1;
}
