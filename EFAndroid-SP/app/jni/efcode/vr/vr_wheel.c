/*
===========================================================================
AwayTeamEF VR: the weapon wheel (EF_VR only).

This file is part of AwayTeamEF and is licensed under the GNU General
Public License, version 2 or (at your option) any later version.
===========================================================================

See vr_wheel.h for who calls what. Compiled into the engine target (an INTERFACE source of
ef_vr), with the client's headers.

Layout, in the 2D canvas' 640x480 virtual frame, centred (SCR_HOR_CENTER | SCR_VERT_CENTER):
the weapons sit on a ring around the canvas' centre, the first one at the top and the rest
clockwise in the game's own order. The middle is the cancel zone. The renderer shows the canvas
on a world-locked quad while the wheel is open (VR_QUAD_WHEEL), so the weapon hand's ray is
intersected with that quad, as last submitted, and turned back into canvas coordinates.

Slow motion is a factor on the single-player simulation clock only (sp_bridge.cpp scales its
real-time delta). It is not a cvar and nothing is saved: closing the wheel for any reason
brings the factor back to exactly 1. It is never below 1 in Holomatch or with a co-op session
(host with sp_coop set, or guest), checked every frame.
*/

#include "../client/client.h"
#include "vr.h"
#include "vr_input.h"
#include "vr_wheel.h"

int VR_HM_Active( void );	// vr_hm.c
#ifdef EF_COOP
int CoopNet_GuestActive( void );	// sys_coop_net.c
int CoopNet_HostHasGuest( void );
#endif

#define WHEEL_RING			150.0f	// radius of the ring of weapons (virtual units)
#define WHEEL_TILE			50.0f	// a weapon's tile
#define WHEEL_CANCEL		52.0f	// the middle: letting go there cancels
#define WHEEL_REACH			330.0f	// beyond this from the middle nothing is picked
#define WHEEL_RAMP_IN		150		// ms to slow time down
#define WHEEL_RAMP_OUT		100		// ms back to normal after a pick or a cancel
#define WHEEL_INPUT_STALE	250		// ms without an input frame: VR went away, close

extern int scr_placement;	// cl_scrn.c

#define HOVER_NONE		-1
#define HOVER_CANCEL	-2

static struct {
	qboolean	registered;
	cvar_t		*slowMotion;
	cvar_t		*open;			// vr_wheelOpen (read-only): the renderer places the quad for it
	cvar_t		*leftHanded;
	cvar_t		*debugStick;

	qboolean	isOpen;
	int			openedAt;
	int			lastUpdate;		// Sys_Milliseconds of the last input frame while open
	vrWheelList_t list;
	int			hover;			// index into list, or HOVER_*
	int			lastPointer;	// the hand's own pick last frame
	int			lastStick;		// the stick's own pick last frame
	qboolean	pointerOnQuad;
	float		pointer[2];		// where the hand's ray meets the canvas (virtual units)

	// the single-player clock: a smooth ramp between two speeds
	float		rampFrom, rampTo;
	int			rampStart, rampLen;
	qboolean	slowed;			// this opening slowed time
} vw;

static void VR_Wheel_Register( void ) {
	vw.slowMotion = Cvar_Get( "vr_wheelSlowMotion", "0.2", CVAR_ARCHIVE );
	Cvar_CheckRange( vw.slowMotion, 0.05f, 1.0f, qfalse );
	Cvar_SetDescription( vw.slowMotion, "VR: how fast time runs while the weapon wheel is open, in single player "
		"only (1 = no slow motion). Co-op and Holomatch always run at normal speed." );
	vw.open = Cvar_Get( "vr_wheelOpen", "0", CVAR_ROM );
	vw.leftHanded = Cvar_Get( "vr_leftHanded", "0", CVAR_ARCHIVE );
	vw.debugStick = Cvar_Get( "vr_debugWheelStick", "", CVAR_TEMP );
	Cvar_SetDescription( vw.debugStick, "VR debugging: the right stick for the weapon wheel, \"x y\" (-1..1)." );
	vw.rampFrom = vw.rampTo = 1.0f;
	vw.hover = vw.lastPointer = vw.lastStick = HOVER_NONE;
	vw.registered = qtrue;
}

// ---------------------------------------------------------------------------
// which game, and may time slow down
// ---------------------------------------------------------------------------

static qboolean VR_Wheel_Holomatch( void ) {
	return Com_Holomatch() || VR_HM_Active();
}

static qboolean VR_Wheel_Coop( void ) {
	if ( Cvar_VariableIntegerValue( "sp_coop" ) || Cvar_VariableString( "coop_connect" )[0] ) {
		return qtrue;
	}
#ifdef EF_COOP
	if ( CoopNet_GuestActive() || CoopNet_HostHasGuest() ) {
		return qtrue;
	}
#endif
	return qfalse;
}

static qboolean VR_Wheel_SlowAllowed( void ) {
	return !VR_Wheel_Holomatch() && !VR_Wheel_Coop() && vw.slowMotion->value < 1.0f;
}

static float VR_Wheel_RampNow( int now ) {
	float f;

	if ( vw.rampLen <= 0 || now - vw.rampStart >= vw.rampLen ) {
		return vw.rampTo;
	}
	f = (float)( now - vw.rampStart ) / (float)vw.rampLen;
	if ( f < 0.0f ) {
		f = 0.0f;
	}
	f = f * f * ( 3.0f - 2.0f * f );
	return vw.rampFrom + ( vw.rampTo - vw.rampFrom ) * f;
}

static void VR_Wheel_SetSpeed( float to, int ms ) {
	const int now = Sys_Milliseconds();

	vw.rampFrom = VR_Wheel_RampNow( now );
	vw.rampTo = to;
	vw.rampStart = now;
	vw.rampLen = ms;
}

// ---------------------------------------------------------------------------
// the weapons
// ---------------------------------------------------------------------------

// Holomatch weapon numbers 1..12 (the game modules' bg_public.h): their HUD icons and names
#define HM_WEAPONS		13
#define HM_STAT_HEALTH	0
#define HM_STAT_WEAPONS	2

static const char *const vr_hmIcons[HM_WEAPONS] = {
	NULL, "icons/w_icon_phaser", "icons/w_icon_rifle", "icons/w_icon_imod", "icons/w_icon_scav",
	"icons/w_icon_stasis", "icons/w_icon_grenade", "icons/w_icon_tetrion", "icons/w_icon_quantum",
	"icons/w_icon_dreadnought", "icons/w_icon_hypo", "icons/w_icon_borg_assimilator", "icons/w_icon_borg_weapon"
};
static const char *const vr_hmNames[HM_WEAPONS] = {
	NULL, "Phaser", "Compression Rifle", "I-MOD", "Scavenger Rifle", "Stasis Weapon", "Grenade Launcher",
	"Tetryon Disruptor", "Photon Burst", "Arc Welder", "Hypo", "Assimilator", "Borg Weapon"
};

static int VR_Wheel_HMList( vrWheelList_t *l ) {
	const playerState_t *ps = &cl.snap.ps;
	int w;

	Com_Memset( l, 0, sizeof( *l ) );
	if ( !cl.snap.valid ) {
		return 0;
	}
	l->current = ps->weapon;
	l->dead = ps->stats[HM_STAT_HEALTH] <= 0;
	l->locked = ps->clientNum != clc.clientNum;	// following someone
	l->noAmmoIcon = re.RegisterShaderNoMip( "icons/noammo" );
	for ( w = 1; w < HM_WEAPONS && l->count < VR_WHEEL_MAX; w++ ) {
		if ( !( ps->stats[HM_STAT_WEAPONS] & ( 1 << w ) ) ) {
			continue;
		}
		l->weapon[l->count] = w;
		l->icon[l->count] = re.RegisterShaderNoMip( vr_hmIcons[w] );
		l->noAmmo[l->count] = ps->ammo[w] == 0;
		Q_strncpyz( l->name[l->count], vr_hmNames[w], sizeof( l->name[0] ) );
		l->count++;
	}
	return 1;
}

static int VR_Wheel_GetList( vrWheelList_t *l ) {
	if ( VR_Wheel_Holomatch() ) {
		return VR_Wheel_HMList( l );
	}
	return SP_VR_WheelWeapons( l );
}

// a reason not to have the wheel up now, or NULL
static const char *VR_Wheel_Blocked( const vrWheelList_t *l ) {
	if ( clc.state != CA_ACTIVE ) {
		return "not in a level";
	}
	if ( !Cvar_VariableIntegerValue( "vr_active" ) ) {
		return "VR off";
	}
	if ( Key_GetCatcher() ) {
		return "menu or console";
	}
	if ( Cvar_VariableIntegerValue( "vr_uiState" ) ) {
		return "cutscene or menu";
	}
	if ( Cvar_VariableIntegerValue( "cg_cameraActive" ) || Cvar_VariableIntegerValue( "skippingCinematic" ) ) {
		return "cutscene";
	}
	if ( l->dead ) {
		return "dead";
	}
	if ( l->locked ) {
		return "can't switch now";
	}
	if ( l->count <= 0 ) {
		return "no weapons";
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// open and close
// ---------------------------------------------------------------------------

static int VR_Wheel_Hand( void ) {
	return vw.leftHanded->integer ? VR_HAND_LEFT : VR_HAND_RIGHT;
}

static void VR_Wheel_Shut( int pick, int rampMs, const char *why ) {
	const int now = Sys_Milliseconds();

	if ( !vw.isOpen ) {
		return;
	}
	vw.isOpen = qfalse;
	Cvar_Set( "vr_wheelOpen", "0" );
	if ( vw.slowed ) {
		VR_Wheel_SetSpeed( 1.0f, rampMs );
		vw.slowed = qfalse;
	}
	if ( pick && vw.hover >= 0 && vw.hover < vw.list.count ) {
		const int w = vw.list.weapon[vw.hover];

		if ( w == vw.list.current ) {
			Com_Printf( "VR wheel: kept weapon %d (%s) after %d ms\n", w, vw.list.name[vw.hover], now - vw.openedAt );
		} else {
			Com_Printf( "VR wheel: picked weapon %d (%s) after %d ms\n", w, vw.list.name[vw.hover], now - vw.openedAt );
			Cbuf_AddText( va( "weapon %d\n", w ) );
		}
	} else {
		Com_Printf( "VR wheel: closed, no pick (%s) after %d ms\n", why, now - vw.openedAt );
	}
	vw.hover = vw.lastPointer = vw.lastStick = HOVER_NONE;
}

int VR_Wheel_Open( void ) {
	const char *blocked;

	if ( !vw.registered ) {
		VR_Wheel_Register();
	}
	if ( vw.isOpen ) {
		return 1;
	}
	if ( !VR_Wheel_GetList( &vw.list ) ) {
		Com_Printf( "VR wheel: no weapon list\n" );
		return 0;
	}
	blocked = VR_Wheel_Blocked( &vw.list );
	if ( blocked ) {
		Com_Printf( "VR wheel: not opened (%s)\n", blocked );
		return 0;
	}
	vw.isOpen = qtrue;
	vw.openedAt = vw.lastUpdate = Sys_Milliseconds();
	vw.hover = vw.lastPointer = vw.lastStick = HOVER_NONE;
	vw.pointerOnQuad = qfalse;
	Cvar_Set( "vr_wheelOpen", "1" );
	vw.slowed = VR_Wheel_SlowAllowed();
	if ( vw.slowed ) {
		VR_Wheel_SetSpeed( vw.slowMotion->value, WHEEL_RAMP_IN );
	}
	VR_Haptic( VR_Wheel_Hand(), 0.3f, 0.02f );
	Com_Printf( "VR wheel: open, %d weapons, current %d, %s\n", vw.list.count, vw.list.current,
		vw.slowed ? va( "time x%.2f", vw.slowMotion->value ) :
		VR_Wheel_Holomatch() ? "time unchanged (Holomatch)" :
		VR_Wheel_Coop() ? "time unchanged (co-op)" : "time unchanged" );
	return 1;
}

void VR_Wheel_Close( int pick, const char *why ) {
	VR_Wheel_Shut( pick, WHEEL_RAMP_OUT, why );
}

void VR_Wheel_Reset( const char *why ) {
	if ( !vw.registered ) {
		return;
	}
	VR_Wheel_Shut( 0, 0, why );
	VR_Wheel_SetSpeed( 1.0f, 0 );
	vw.slowed = qfalse;
}

int VR_Wheel_IsOpen( void ) {
	return vw.isOpen;
}

float VR_Wheel_TimeScale( void ) {
	const int now = Sys_Milliseconds();

	if ( !vw.registered ) {
		return 1.0f;
	}
	if ( vw.isOpen && now - vw.lastUpdate > WHEEL_INPUT_STALE ) {
		VR_Wheel_Reset( "no VR input" );
	}
	if ( vw.rampTo != 1.0f || VR_Wheel_RampNow( now ) != 1.0f ) {
		if ( !VR_Wheel_SlowAllowed() ) {
			// a co-op session started, or Holomatch: normal speed at once
			vw.slowed = qfalse;
			VR_Wheel_SetSpeed( 1.0f, 0 );
		}
	}
	return VR_Wheel_RampNow( now );
}

// ---------------------------------------------------------------------------
// pointing
// ---------------------------------------------------------------------------

// OpenXR (+X right, +Y up, -Z forward) to the game's body frame (+X forward, +Y left, +Z up)
static void VR_Wheel_FromXr( const float *xr, float *out ) {
	out[0] = -xr[2];
	out[1] = -xr[0];
	out[2] = xr[1];
}

/*
Where the weapon hand's ray meets the 2D layer's quad, if the quad last drawn was of this mode and
is held in the room (not head-locked): canvas pixels (0,0 at the top left, the render size; may be
off the canvas). Used
for the wheel here and for the menu cursor (vr_input_engine.c).
*/
static int VR_Pointer_Trace( int wantMode, float *px, float *py, float *xrFrom, float *xrTo ) {
	vrQuadMode_t mode;
	vrBodyHand_t hand;
	float centre[3], yaw, pitch, size[2], head[3], rel[3], xr[3];
	vec3_t q, right, up, normal, d;
	float scale, denom, t, lx, ly;
	int i;

	if ( !VR_GetLastQuad( &mode, centre, &yaw, size ) || mode != wantMode ) {
		return qfalse;
	}
	if ( !VR_EngineBodyHand( VR_BODY_WEAPON, &hand ) ) {
		return qfalse;
	}
	scale = Cvar_VariableValue( "vr_worldScale" );
	if ( scale <= 0.0f ) {
		scale = 32.0f;
	}

	// the quad in the hand's frame: relative to the head origin, scaled
	VR_GetHeadOrigin( head );
	for ( i = 0; i < 3; i++ ) {
		rel[i] = centre[i] - head[i];
	}
	VR_Wheel_FromXr( rel, q );
	VectorScale( q, scale, q );
	// the quad's axes: turned by its yaw, then tilted by its pitch (a soft-following menu faces a
	// head that looks up or down; every other quad is upright, pitch 0)
	pitch = VR_GetLastQuadPitch();
	xr[0] = cos( yaw ); xr[1] = 0.0f; xr[2] = -sin( yaw );
	VR_Wheel_FromXr( xr, right );
	xr[0] = sin( pitch ) * sin( yaw ); xr[1] = cos( pitch ); xr[2] = sin( pitch ) * cos( yaw );
	VR_Wheel_FromXr( xr, up );
	xr[0] = cos( pitch ) * sin( yaw ); xr[1] = -sin( pitch ); xr[2] = cos( pitch ) * cos( yaw );	// faces the head
	VR_Wheel_FromXr( xr, normal );

	denom = DotProduct( hand.axis[0], normal );
	if ( denom > -0.05f ) {
		return qfalse;	// pointing away, or along it
	}
	VectorSubtract( q, hand.origin, d );
	t = DotProduct( d, normal ) / denom;
	if ( t <= 0.0f ) {
		return qfalse;
	}
	VectorMA( hand.origin, t, hand.axis[0], d );
	if ( xrFrom && xrTo ) {
		// both ends in the reference space (OpenXR axes, metres): the game's frame turned back
		// and the head origin added again
		vec3_t p;

		VectorCopy( hand.origin, p );
		xrFrom[0] = -p[1] / scale + head[0];
		xrFrom[1] = p[2] / scale + head[1];
		xrFrom[2] = -p[0] / scale + head[2];
		VectorCopy( d, p );
		xrTo[0] = -p[1] / scale + head[0];
		xrTo[1] = p[2] / scale + head[1];
		xrTo[2] = -p[0] / scale + head[2];
	}
	VectorSubtract( d, q, d );
	lx = DotProduct( d, right ) / scale;	// metres on the quad
	ly = DotProduct( d, up ) / scale;

	*px = ( lx / size[0] + 0.5f ) * cls.glconfig.vidWidth;
	*py = ( 0.5f - ly / size[1] ) * cls.glconfig.vidHeight;
	return qtrue;	// it may be beyond the quad's edges: the caller decides
}

int VR_Pointer_OnCanvas( int wantMode, float *px, float *py ) {
	return VR_Pointer_Trace( wantMode, px, py, NULL, NULL );
}

// the same, with the two ends of the ray for drawing it: the weapon hand and the hit point
int VR_Pointer_Laser( int wantMode, float *px, float *py, float xrFrom[3], float xrTo[3] ) {
	return VR_Pointer_Trace( wantMode, px, py, xrFrom, xrTo );
}

// where the hand's ray meets the wheel's quad, in the virtual frame the wheel is drawn in
static qboolean VR_Wheel_PointerOnCanvas( float *vx, float *vy ) {
	float px, py;
	int placement;

	if ( !VR_Pointer_OnCanvas( VR_QUAD_WHEEL, &px, &py ) ) {
		return qfalse;
	}
	placement = scr_placement;
	SCR_SetScreenPlacement( SCR_HOR_CENTER | SCR_VERT_CENTER );
	SCR_AdjustTo640( &px, &py, NULL, NULL );
	SCR_SetScreenPlacement( placement );
	*vx = px;
	*vy = py;
	return qtrue;
}

// the segment at a direction from the middle (dx right, dy down), the first at the top, clockwise
static int VR_Wheel_Segment( float dx, float dy ) {
	float a = atan2( dx, -dy );
	int n = vw.list.count;
	int s;

	if ( n <= 0 ) {
		return HOVER_NONE;
	}
	if ( a < 0.0f ) {
		a += 2.0f * (float)M_PI;
	}
	s = (int)( a / ( 2.0f * (float)M_PI / n ) + 0.5f );
	return s % n;
}

void VR_Wheel_Update( const float stick[2], int inPlay ) {
	vrWheelList_t list;
	const char *blocked;
	int pointer = HOVER_NONE, stickPick = HOVER_NONE, prev = vw.hover;
	float sx = stick[0], sy = stick[1];

	if ( !vw.isOpen ) {
		return;
	}
	vw.lastUpdate = Sys_Milliseconds();
	if ( !inPlay ) {
		VR_Wheel_Reset( "left play" );
		return;
	}
	// the list again: ammo and weapons can change while it's open (time only slows)
	if ( !VR_Wheel_GetList( &list ) ) {
		VR_Wheel_Reset( "weapon list gone" );
		return;
	}
	blocked = VR_Wheel_Blocked( &list );
	if ( blocked ) {
		VR_Wheel_Reset( blocked );
		return;
	}
	if ( list.count != vw.list.count || memcmp( list.weapon, vw.list.weapon, sizeof( list.weapon ) ) ) {
		vw.hover = vw.lastPointer = vw.lastStick = HOVER_NONE;	// the ring changed under the pointer
		prev = HOVER_NONE;
	}
	vw.list = list;

	// the hand
	vw.pointerOnQuad = VR_Wheel_PointerOnCanvas( &vw.pointer[0], &vw.pointer[1] );
	if ( vw.pointerOnQuad ) {
		const float dx = vw.pointer[0] - 320.0f, dy = vw.pointer[1] - 240.0f;
		const float r = sqrt( dx * dx + dy * dy );

		pointer = r < WHEEL_CANCEL ? HOVER_CANCEL : r > WHEEL_REACH ? HOVER_NONE : VR_Wheel_Segment( dx, dy );
	}
	// the stick (or its stand-in for testing)
	if ( vw.debugStick->string[0] ) {
		sscanf( vw.debugStick->string, "%f %f", &sx, &sy );
	}
	if ( sx * sx + sy * sy > 0.6f * 0.6f ) {
		stickPick = VR_Wheel_Segment( sx, -sy );	// stick up = the top
	}

	// whichever moved last decides: a new place under the hand, or a new stick direction
	if ( pointer != vw.lastPointer ) {
		vw.hover = pointer;
	}
	if ( stickPick != HOVER_NONE && stickPick != vw.lastStick ) {
		vw.hover = stickPick;
	}
	vw.lastPointer = pointer;
	vw.lastStick = stickPick;

	if ( vw.hover != prev && vw.hover >= 0 ) {
		VR_Haptic( VR_Wheel_Hand(), 0.2f, 0.012f );	// a tick per segment
	}
}

// ---------------------------------------------------------------------------
// drawing (canvas, virtual 640x480, centred)
// ---------------------------------------------------------------------------

static void VR_Wheel_Rect( float x, float y, float w, float h, const float *rgba ) {
	SCR_FillRect( x, y, w, h, rgba );
}

// a filled disc from horizontal strips, whole pixel rows each so none overlap or leave a gap
static void VR_Wheel_Disc( float cx, float cy, float r, const float *rgba ) {
	float x = cx - r, y = cy - r, w = 2.0f * r, h = 2.0f * r;
	int top, bottom, row;
	const int step = 2;

	SCR_AdjustFrom640( &x, &y, &w, &h );
	top = (int)y;
	bottom = (int)( y + h + 0.5f );
	re.SetColor( rgba );
	for ( row = top; row < bottom; row += step ) {
		const float t = ( row + step * 0.5f - ( y + h * 0.5f ) ) / ( h * 0.5f );
		float hw;

		if ( t <= -1.0f || t >= 1.0f ) {
			continue;
		}
		hw = 0.5f * w * sqrt( 1.0f - t * t );
		re.DrawStretchPic( (int)( x + w * 0.5f - hw + 0.5f ), row, (int)( 2.0f * hw + 0.5f ), step, 0, 0, 0, 0, cls.whiteShader );
	}
	re.SetColor( NULL );
}

// a dotted circle
static void VR_Wheel_Ring( float cx, float cy, float r, float dot, int dots, const float *rgba ) {
	int i;

	for ( i = 0; i < dots; i++ ) {
		const float a = 2.0f * (float)M_PI * i / dots;
		VR_Wheel_Rect( cx + r * sin( a ) - dot * 0.5f, cy - r * cos( a ) - dot * 0.5f, dot, dot, rgba );
	}
}

// the console font, at any size (each glyph is half as wide as it is tall)
static void VR_Wheel_Text( float cx, float y, float h, const char *s, const float *rgba ) {
	const float w = h * 0.5f;
	float x = cx - strlen( s ) * w * 0.5f;

	re.SetColor( rgba );
	for ( ; *s; s++, x += w ) {
		const int ch = *s & 255;
		float ax = x, ay = y, aw = w, ah = h;

		if ( ch == ' ' ) {
			continue;
		}
		SCR_AdjustFrom640( &ax, &ay, &aw, &ah );
		re.DrawStretchPic( ax, ay, aw, ah, ( ch & 15 ) * 0.0625f, ( ch >> 4 ) * 0.0625f,
			( ch & 15 ) * 0.0625f + 0.03125f, ( ch >> 4 ) * 0.0625f + 0.0625f, cls.charSetShader );
	}
	re.SetColor( NULL );
}

void VR_Wheel_Draw( void ) {
	static const float back[4] = { 0.02f, 0.05f, 0.10f, 0.62f };
	static const float ringCol[4] = { 0.55f, 0.75f, 1.0f, 0.55f };
	static const float middle[4] = { 0.0f, 0.02f, 0.05f, 0.70f };
	static const float middleHot[4] = { 0.55f, 0.12f, 0.10f, 0.80f };
	static const float tile[4] = { 0.10f, 0.16f, 0.26f, 0.88f };
	static const float tileEmpty[4] = { 0.26f, 0.08f, 0.08f, 0.88f };
	static const float tileHot[4] = { 0.95f, 0.62f, 0.14f, 0.95f };
	static const float currentCol[4] = { 0.40f, 0.90f, 1.0f, 1.0f };
	static const float spoke[4] = { 0.95f, 0.62f, 0.14f, 0.75f };
	static const float dimIcon[4] = { 0.5f, 0.5f, 0.5f, 0.75f };
	static const float textCol[4] = { 1.0f, 0.85f, 0.55f, 1.0f };
	static const float pointerCol[4] = { 1.0f, 1.0f, 1.0f, 0.95f };
	const float cx = 320.0f, cy = 240.0f;
	const char *label = NULL;
	const int placement = scr_placement;
	int i, n = vw.list.count;

	if ( !vw.isOpen || n <= 0 ) {
		return;
	}
	SCR_SetScreenPlacement( SCR_HOR_CENTER | SCR_VERT_CENTER );

	VR_Wheel_Disc( cx, cy, WHEEL_RING + WHEEL_TILE * 0.95f, back );
	VR_Wheel_Ring( cx, cy, WHEEL_RING + WHEEL_TILE * 0.95f, 3.0f, 72, ringCol );
	VR_Wheel_Disc( cx, cy, WHEEL_CANCEL, vw.hover == HOVER_CANCEL ? middleHot : middle );
	VR_Wheel_Ring( cx, cy, WHEEL_CANCEL, 2.0f, 36, ringCol );

	for ( i = 0; i < n; i++ ) {
		const float a = 2.0f * (float)M_PI * i / n;
		const qboolean hot = i == vw.hover;
		const float s = hot ? WHEEL_TILE * 1.3f : WHEEL_TILE;
		const float tx = cx + WHEEL_RING * sin( a ), ty = cy - WHEEL_RING * cos( a );

		if ( hot ) {
			// a dotted spoke from the middle out to the picked weapon
			float r;
			for ( r = WHEEL_CANCEL + 8.0f; r < WHEEL_RING - s * 0.6f; r += 9.0f ) {
				VR_Wheel_Rect( cx + r * sin( a ) - 2.0f, cy - r * cos( a ) - 2.0f, 4.0f, 4.0f, spoke );
			}
		}
		VR_Wheel_Rect( tx - s * 0.5f - 3.0f, ty - s * 0.5f - 3.0f, s + 6.0f, s + 6.0f,
			hot ? tileHot : vw.list.noAmmo[i] ? tileEmpty : tile );
		if ( vw.list.icon[i] ) {
			re.SetColor( vw.list.noAmmo[i] ? dimIcon : NULL );
			SCR_DrawPic( tx - s * 0.5f, ty - s * 0.5f, s, s, vw.list.icon[i] );
			re.SetColor( NULL );
		}
		if ( vw.list.noAmmo[i] && vw.list.noAmmoIcon ) {
			SCR_DrawPic( tx - s * 0.5f, ty - s * 0.5f, s, s, vw.list.noAmmoIcon );
		}
		if ( vw.list.weapon[i] == vw.list.current ) {
			// the weapon in hand: a bar under its tile
			VR_Wheel_Rect( tx - s * 0.5f, ty + s * 0.5f + 5.0f, s, 4.0f, currentCol );
		}
		if ( hot ) {
			label = vw.list.name[i][0] ? vw.list.name[i] : NULL;
		}
	}

	if ( vw.hover == HOVER_CANCEL ) {
		label = "Cancel";
	}
	if ( label ) {
		VR_Wheel_Text( cx, cy - 8.0f, 16.0f, label, textCol );
	}
	if ( vw.pointerOnQuad ) {
		VR_Wheel_Rect( vw.pointer[0] - 3.0f, vw.pointer[1] - 3.0f, 6.0f, 6.0f, pointerCol );
	}
	SCR_SetScreenPlacement( placement );
}
