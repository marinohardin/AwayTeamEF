/*
===========================================================================
AwayTeamEF VR: the renderer's side of the VR build (EF_VR only).

This file is part of AwayTeamEF and is licensed under the GNU General
Public License, version 2 or (at your option) any later version.
===========================================================================

- the vr_* cvars and the vr_recenter command
- per frame: what kind of frame this is (game, menu, cutscene), VR_BeginFrame from
  RE_BeginFrame, and the layers for VR_EndFrame from RE_EndFrame
- per world scene: the view is drawn once per eye (RE_RenderScene calls R_VR_SetupEye),
  each eye into its own OpenXR image, with the eye's pose and asymmetric field of view

docs/vr/RENDERING.md explains the choices. Without EF_VR this file compiles to nothing.
*/

#ifdef EF_VR

#include "tr_local.h"
#include "../vr/vr.h"

cvar_t	*vr_enable;
cvar_t	*vr_worldScale;
cvar_t	*vr_msaa;
static cvar_t	*vr_resolutionScale;
static cvar_t	*vr_refreshRate;
static cvar_t	*vr_hudDistance;
static cvar_t	*vr_hudWidth;
static cvar_t	*vr_hudHeight;
static cvar_t	*vr_hudFollow;
static cvar_t	*vr_hudFollowAngle;
static cvar_t	*vr_menuDistance;
static cvar_t	*vr_menuWidth;
static cvar_t	*vr_menuHeadLocked;
static cvar_t	*vr_menuFollow;
static cvar_t	*vr_menuFollowLag;
static cvar_t	*vr_menuDim;
static cvar_t	*vr_cutsceneScreen;
static cvar_t	*vr_theaterDistance;
static cvar_t	*vr_theaterWidth;
static cvar_t	*vr_theaterDim;
static cvar_t	*vr_uiState;		// set by the client: 1 = a menu is up, 2 = a cinematic plays,
								// 3 = a menu over the live world, 4 = the theater screen (Holomatch)
static cvar_t	*vr_wheelOpen;		// set by the engine's weapon wheel (vr_wheel.c) while it's open
static cvar_t	*vr_wheelDistance;
static cvar_t	*vr_wheelWidth;
static cvar_t	*vr_wheelHeight;
static cvar_t	*vr_debugCapture;
static cvar_t	*vr_debugFrames;
static cvar_t	*vr_menuLaser;
static cvar_t	*vr_layoutVersion;
static cvar_t	*vr_menuSettle;
static cvar_t	*vr_active;		// read-only, for the client: 1 while frames go to the headset
static cvar_t	*vr_debugHeadTurn;	// "yaw pitch roll": a pretend head turn, for testing

typedef enum {
	VRF_GAME,		// the world in stereo, the HUD on its quad
	VRF_MENU,		// a menu over the frozen, dimmed world
	VRF_MENU_LIVE,	// a menu over the live, dimmed world (Holomatch: the match goes on)
	VRF_THEATER		// a cutscene camera or a cinematic on a big screen
} vrFrameKind_t;

static qboolean r_vrFrame;		// R_VR_BeginFrame ran VR_BeginFrame; R_VR_EndFrame is due
static qboolean r_vrWarnedSmp;
static qboolean r_vrExitHandled;	// the runtime ended the session: flat or quit was queued
static vrFrameKind_t r_vrKind;
static vrFrameKind_t r_vrPrevKind = VRF_GAME;
static qboolean r_vrHold;		// this frame is one of those
static int r_vrMenuSettle;		// frames left of holding the HUD at a menu's opening (see R_VR_BeginFrame)
static int r_vrThumbWait;		// frames left that the hold waits for the save thumbnail (see R_VR_SceneEyes)

/*
The in-game menu's first frames aren't a clean picture: the first one is the menu on an opaque black
background and the next one has nothing drawn at all (the single-player bridge hides the menu for one
frame to capture the save thumbnail), and only then does the menu settle. Shown as they come, that is a
black flash and a blank frame, one flicker every time the menu opens (K4). So for the first
VR_MENU_SETTLE frames after a game frame the world stays frozen and the 2D layer keeps showing the last
image it had (the HUD), no dimming and no laser, and then the menu comes in whole.
*/
#define VR_MENU_SETTLE	3	// vr_menuSettle's default

/*
The save thumbnail. The single-player bridge asks for it on the frame the game pauses for the menu
(RE_RequestSaveThumb), hides the menu on that frame and redraws the paused game, and the thumbnail is
read from the 2D canvas at the end of the frame (RB_CaptureSaveThumb), as on the flat screen. But in VR
the world goes into the eye images, not the canvas, and under a menu it isn't drawn at all, so the
canvas held the HUD over transparent black: every thumbnail came out black. So on that one frame the
world is drawn flat into the canvas, under the HUD, as the flat screen draws it. The headset doesn't see
it: the frame is one of the held ones (the 2D layer re-shows its last image). The hold lasts at least
until the capture frame, up to VR_THUMB_WAIT frames after the menu opens, even with vr_menuSettle 0 or 1.
*/
#define VR_THUMB_WAIT	4

static void R_VR_Recenter_f( void ) {
	VR_Recenter();
}

static void R_VR_DebugEndSession_f( void ) {
	VR_DebugEndSession();
}

static void R_VR_Screenshot_f( void ) {
	if ( !vr_debugCapture->integer ) {
		ri.Printf( PRINT_ALL, "vr_screenshot needs vr_debugCapture 1 (and a vid_restart)\n" );
		return;
	}
	vk_vr_request_capture( ri.Cmd_Argc() > 1 ? atoi( ri.Cmd_Argv( 1 ) ) : 1 );
}

static cvar_t *R_VR_Cvar( const char *name, const char *value, int flags, const char *min, const char *max,
	int type, const char *description ) {
	cvar_t *cv = ri.Cvar_Get( name, value, flags );
	ri.Cvar_CheckRange( cv, min, max, type );
	ri.Cvar_SetDescription( cv, description );
	return cv;
}

void R_VR_Register( void ) {
#ifdef __ANDROID__
	const char *enableDefault = "1";	// the Quest build
#else
	const char *enableDefault = "0";	// a desktop VR build only uses a headset when asked
#endif

	vr_enable = R_VR_Cvar( "vr_enable", enableDefault, CVAR_ARCHIVE_ND | CVAR_LATCH, "0", "1", CV_INTEGER,
		"Render to an OpenXR headset when one is present. Requires vid_restart." );
	vr_worldScale = R_VR_Cvar( "vr_worldScale", "32", CVAR_ARCHIVE_ND, "8", "128", CV_FLOAT,
		"VR: game units per metre of head movement (32: a standing player's eyes, 56 units up, are 1.75 m)." );
	vr_msaa = R_VR_Cvar( "vr_msaa", "4", CVAR_ARCHIVE_ND | CVAR_LATCH, "0", "8", CV_INTEGER,
		"VR: multisample anti-aliasing of the eye images (0, 2, 4 or 8). Requires vid_restart." );
	vr_resolutionScale = R_VR_Cvar( "vr_resolutionScale", "1.0", CVAR_ARCHIVE_ND | CVAR_LATCH, "0.5", "1.5", CV_FLOAT,
		"VR: eye image size, times the headset's recommended size. Requires vid_restart." );
	vr_refreshRate = R_VR_Cvar( "vr_refreshRate", "72", CVAR_ARCHIVE_ND | CVAR_LATCH, "0", "144", CV_FLOAT,
		"VR: display refresh rate to ask the headset for, in Hz (0: its default). Requires vid_restart." );

	vr_hudDistance = R_VR_Cvar( "vr_hudDistance", "1.2", CVAR_ARCHIVE_ND, "0.5", "5", CV_FLOAT,
		"VR: distance of the HUD in metres." );
	vr_hudWidth = R_VR_Cvar( "vr_hudWidth", "1.2", CVAR_ARCHIVE_ND, "0.3", "5", CV_FLOAT,
		"VR: width of the HUD in metres." );
	vr_hudHeight = R_VR_Cvar( "vr_hudHeight", "-0.1", CVAR_ARCHIVE_ND, "-2", "2", CV_FLOAT,
		"VR: height of the HUD's centre above eye level, in metres." );
	vr_hudFollow = R_VR_Cvar( "vr_hudFollow", "2", CVAR_ARCHIVE_ND, "0", "2", CV_INTEGER,
		"VR: the HUD 0 stays where it was placed, 1 is locked to the head, 2 turns after the head once it has looked away (vr_hudFollowAngle)." );
	vr_hudFollowAngle = R_VR_Cvar( "vr_hudFollowAngle", "30", CVAR_ARCHIVE_ND, "0", "90", CV_FLOAT,
		"VR: degrees the head may turn away before the HUD follows it (vr_hudFollow 2)." );

	vr_menuDistance = R_VR_Cvar( "vr_menuDistance", "0.7", CVAR_ARCHIVE_ND, "0.4", "5", CV_FLOAT,
		"VR: distance of the menu screen in metres." );
	vr_menuWidth = R_VR_Cvar( "vr_menuWidth", "0.9", CVAR_ARCHIVE_ND, "0.3", "6", CV_FLOAT,
		"VR: width of the menu screen in metres." );
	vr_menuHeadLocked = R_VR_Cvar( "vr_menuHeadLocked", "0", CVAR_ARCHIVE_ND, "0", "1", CV_INTEGER,
		"VR: 1 = the menu is locked to the head (over vr_menuFollow)." );
	vr_menuFollow = R_VR_Cvar( "vr_menuFollow", "1", CVAR_ARCHIVE_ND, "0", "1", CV_INTEGER,
		"VR: 1 = the menu follows the head smoothly, a little behind it; 0 = it stays where it opened." );
	vr_menuFollowLag = R_VR_Cvar( "vr_menuFollowLag", "0.25", CVAR_ARCHIVE_ND, "0", "2", CV_FLOAT,
		"VR: how far the following menu lags behind the head: seconds to close most of the gap (0 = none)." );
	vr_menuDim = R_VR_Cvar( "vr_menuDim", "0.6", CVAR_ARCHIVE_ND, "0", "1", CV_FLOAT,
		"VR: how much the frozen world behind a menu is darkened (0 none, 1 black)." );

	// The layer cvars are archived, so a config saved by an older build still holds the old default
	// distances and sizes, and a new default would never reach it. Version 1 of the layout moved the
	// HUD and the menu closer (K10): once, those four go to the new defaults. Version 2 brought the
	// menu to about arm's length (0.7 m, 0.9 m wide) and made it follow the head.
	vr_layoutVersion = R_VR_Cvar( "vr_layoutVersion", "0", CVAR_ARCHIVE_ND, "0", "100", CV_INTEGER,
		"VR: which defaults the HUD and menu placement cvars were last reset to (set by the game)." );
	if ( vr_layoutVersion->integer < 1 ) {
		ri.Cvar_Set( "vr_hudDistance", "1.2" );
		ri.Cvar_Set( "vr_hudWidth", "1.2" );
	}
	if ( vr_layoutVersion->integer < 2 ) {
		ri.Cvar_Set( "vr_menuDistance", "0.7" );
		ri.Cvar_Set( "vr_menuWidth", "0.9" );
		ri.Cvar_Set( "vr_layoutVersion", "2" );
	}

	vr_cutsceneScreen = R_VR_Cvar( "vr_cutsceneScreen", "1", CVAR_ARCHIVE_ND, "0", "1", CV_INTEGER,
		"VR: cutscene cameras and cinematics on a virtual screen (1), or cutscene cameras in stereo (0)." );
	vr_theaterDistance = R_VR_Cvar( "vr_theaterDistance", "4.0", CVAR_ARCHIVE_ND, "1", "20", CV_FLOAT,
		"VR: distance of the cutscene screen in metres." );
	vr_theaterWidth = R_VR_Cvar( "vr_theaterWidth", "4.4", CVAR_ARCHIVE_ND, "1", "30", CV_FLOAT,
		"VR: width of the cutscene screen in metres." );
	vr_theaterDim = R_VR_Cvar( "vr_theaterDim", "0.85", CVAR_ARCHIVE_ND, "0", "1", CV_FLOAT,
		"VR: how much the frozen world around the cutscene screen is darkened (0 none, 1 black)." );

	vr_wheelDistance = R_VR_Cvar( "vr_wheelDistance", "1.0", CVAR_ARCHIVE_ND, "0.4", "3", CV_FLOAT,
		"VR: distance of the weapon wheel in metres (it shows on the HUD's layer, placed when it opens)." );
	vr_wheelWidth = R_VR_Cvar( "vr_wheelWidth", "1.4", CVAR_ARCHIVE_ND, "0.3", "4", CV_FLOAT,
		"VR: width of the HUD's layer while the weapon wheel is open, in metres." );
	vr_wheelHeight = R_VR_Cvar( "vr_wheelHeight", "-0.15", CVAR_ARCHIVE_ND, "-2", "2", CV_FLOAT,
		"VR: height of the weapon wheel's centre above eye level, in metres." );
	vr_wheelOpen = ri.Cvar_Get( "vr_wheelOpen", "0", CVAR_ROM );
	vr_uiState = ri.Cvar_Get( "vr_uiState", "0", CVAR_ROM );
	vr_active = ri.Cvar_Get( "vr_active", "0", CVAR_ROM );
	ri.Cvar_Set( "vr_active", "0" );
	vr_debugCapture = R_VR_Cvar( "vr_debugCapture", "0", CVAR_LATCH, "0", "1", CV_INTEGER,
		"VR debugging: make the eye images readable for vr_screenshot. Requires vid_restart." );
	vr_menuSettle = R_VR_Cvar( "vr_menuSettle", "3", CVAR_ARCHIVE_ND, "0", "12", CV_INTEGER,
		"VR: frames the HUD stays up, world frozen, when the in-game menu opens, so the menu's unsettled first frames "
		"aren't shown (0 = show them as they come)." );
	vr_debugFrames = ri.Cvar_Get( "vr_debugFrames", "0", CVAR_TEMP );
	ri.Cvar_SetDescription( vr_debugFrames,
		"VR debugging: 1 = log what every frame submits (the world, the dimmer, the 2D quad, the laser). "
		"With vr_screenshot <n> the captured files are numbered by the same frame number." );
	vr_menuLaser = R_VR_Cvar( "vr_menuLaser", "1", CVAR_ARCHIVE_ND, "0", "1", CV_INTEGER,
		"VR: 1 = a red laser from the weapon hand to the menu cursor while a menu is up." );
	vr_debugHeadTurn = ri.Cvar_Get( "vr_debugHeadTurn", "", 0 );
	ri.Cvar_SetDescription( vr_debugHeadTurn,
		"VR debugging: pretend head turn, \"yaw pitch roll\" in degrees (yaw left, pitch up and roll anticlockwise are positive)." );

	if ( vr_enable->integer ) {
		// VR draws the 2D canvas in the main pass's offscreen image: it needs r_fbo, and an
		// 8-bit colour format for the copy into the 2D layer
		if ( !r_fbo->integer ) {
			ri.Cvar_Set( "r_fbo", "1" );
		}
		if ( r_hdr->integer ) {
			ri.Cvar_Set( "r_hdr", "0" );
		}
	}

	ri.Cmd_AddCommand( "vr_recenter", R_VR_Recenter_f );
	ri.Cmd_AddCommand( "vr_screenshot", R_VR_Screenshot_f );
	ri.Cmd_AddCommand( "vr_debugEndSession", R_VR_DebugEndSession_f );
}

void R_VR_Print( int isError, const char *msg ) {
	ri.Printf( isError ? PRINT_WARNING : PRINT_ALL, "%s", msg );
}

void R_VR_SetOptions( void ) {
	vrOptions_t options;

	options.resolutionScale = vr_resolutionScale ? vr_resolutionScale->value : 1.0f;
	options.refreshRate = vr_refreshRate ? vr_refreshRate->value : 72.0f;
	options.debugCapture = vr_debugCapture ? vr_debugCapture->integer : 0;
	VR_SetOptions( &options );
}

static vrFrameKind_t R_VR_FrameKind( void ) {
	const int ui = vr_uiState ? vr_uiState->integer : 0;

	if ( ui == 1 ) {
		return VRF_MENU;
	}
	if ( ui == 3 ) {
		return VRF_MENU_LIVE;
	}
	if ( ui == 4 ) {
		return VRF_THEATER;	// Holomatch: following a player, the intermission
	}
	// the game's own state, no game-module change: the cgame sets cg_cameraActive while a
	// scripted camera runs, and the client reports cinematics (vr_uiState 2)
	if ( vr_cutsceneScreen->integer && ( ui == 2 || ri.Cvar_VariableIntegerValue( "cg_cameraActive" ) ) ) {
		return VRF_THEATER;
	}
	return VRF_GAME;
}

// vr_debugHeadTurn "yaw pitch roll" (degrees) -> a rotation of the located views (OpenXR axes:
// yaw about +Y, pitch about +X, roll about +Z), applied in that order from the room's frame
static void R_VR_DebugHeadTurn( void ) {
	static int modified = -1;
	float a[3] = { 0, 0, 0 }, qy[4], qp[4], qr[4], t[4], q[4];

	if ( vr_debugHeadTurn->modificationCount == modified ) {
		return;
	}
	modified = vr_debugHeadTurn->modificationCount;
	if ( sscanf( vr_debugHeadTurn->string, "%f %f %f", &a[0], &a[1], &a[2] ) < 1 ||
		( a[0] == 0.0f && a[1] == 0.0f && a[2] == 0.0f ) ) {
		VR_SetDebugHeadTurn( NULL );
		return;
	}
	a[0] = DEG2RAD( a[0] ) * 0.5f;
	a[1] = DEG2RAD( a[1] ) * 0.5f;
	a[2] = DEG2RAD( a[2] ) * 0.5f;
	qy[0] = 0; qy[1] = sin( a[0] ); qy[2] = 0; qy[3] = cos( a[0] );
	qp[0] = sin( a[1] ); qp[1] = 0; qp[2] = 0; qp[3] = cos( a[1] );
	qr[0] = 0; qr[1] = 0; qr[2] = sin( a[2] ); qr[3] = cos( a[2] );
	// t = qy * qp, q = t * qr
	t[0] = qy[3] * qp[0] + qy[0] * qp[3] + qy[1] * qp[2] - qy[2] * qp[1];
	t[1] = qy[3] * qp[1] - qy[0] * qp[2] + qy[1] * qp[3] + qy[2] * qp[0];
	t[2] = qy[3] * qp[2] + qy[0] * qp[1] - qy[1] * qp[0] + qy[2] * qp[3];
	t[3] = qy[3] * qp[3] - qy[0] * qp[0] - qy[1] * qp[1] - qy[2] * qp[2];
	q[0] = t[3] * qr[0] + t[0] * qr[3] + t[1] * qr[2] - t[2] * qr[1];
	q[1] = t[3] * qr[1] - t[0] * qr[2] + t[1] * qr[3] + t[2] * qr[0];
	q[2] = t[3] * qr[2] + t[0] * qr[1] - t[1] * qr[0] + t[2] * qr[3];
	q[3] = t[3] * qr[3] - t[0] * qr[0] - t[1] * qr[1] - t[2] * qr[2];
	VR_SetDebugHeadTurn( q );
}

void R_VR_BeginFrame( void ) {
	float dim;

	r_vrFrame = qfalse;

	if ( VR_GetExitReason() != VR_EXIT_NONE && !r_vrExitHandled ) {
		// the user quit from the headset's system menu, or the headset was lost
		r_vrExitHandled = qtrue;
#ifdef __ANDROID__
		ri.Printf( PRINT_ALL, "VR: the headset session ended, quitting\n" );
		ri.Cmd_ExecuteText( EXEC_APPEND, "quit\n" );
#else
		ri.Printf( PRINT_ALL, "VR: the headset session ended, switching to the flat screen\n" );
		ri.Cmd_ExecuteText( EXEC_APPEND, "vid_restart\n" );	// VR_Init stays flat from now on
#endif
	}

	if ( !vk_vr_ready() || glConfig.smpActive ) {
		if ( vr_active && vr_active->integer ) {
			ri.Cvar_Set( "vr_active", "0" );
		}
	}
	if ( !vk_vr_ready() ) {
		return;
	}
	if ( glConfig.smpActive ) {
		// the eye images are filled and released on one thread; the render thread would
		// submit after VR_EndFrame
		if ( !r_vrWarnedSmp ) {
			ri.Printf( PRINT_WARNING, "VR: needs r_smp 0, rendering flat\n" );
			r_vrWarnedSmp = qtrue;
		}
		return;
	}

	if ( !vr_active->integer ) {
		ri.Cvar_Set( "vr_active", "1" );
	}
	R_VR_DebugHeadTurn();
	r_vrKind = R_VR_FrameKind();
	if ( r_vrKind == VRF_MENU && r_vrPrevKind != VRF_MENU && r_vrPrevKind != VRF_MENU_LIVE && VR_HaveFrozenEyes() ) {
		r_vrMenuSettle = vr_menuSettle->integer;	// a menu opens over a running game
		r_vrThumbWait = VR_THUMB_WAIT;
	} else if ( r_vrKind != VRF_MENU ) {
		r_vrMenuSettle = 0;
		r_vrThumbWait = 0;
	}
	r_vrPrevKind = r_vrKind;
	r_vrHold = ( r_vrMenuSettle > 0 || r_vrThumbWait > 0 ) && VR_HaveCanvas();
	if ( r_vrMenuSettle > 0 ) {
		r_vrMenuSettle--;
	}
	if ( r_vrThumbWait > 0 ) {
		r_vrThumbWait--;
	}
	VR_SetHoldCanvas( r_vrHold );
	// in play the dimmer is the head-in-a-wall fade (vr_move.c)
	dim = r_vrHold ? 0.0f : ( r_vrKind == VRF_MENU || r_vrKind == VRF_MENU_LIVE ) ? vr_menuDim->value :
		r_vrKind == VRF_THEATER ? vr_theaterDim->value : VR_GetComfortFade();
	VR_BeginFrame( r_vrKind == VRF_GAME || r_vrKind == VRF_MENU_LIVE, dim );
	r_vrFrame = qtrue;
}

void R_VR_EndFrame( void ) {
	vrFrameLayers_t layers;

	if ( !r_vrFrame ) {
		return;
	}
	r_vrFrame = qfalse;

	Com_Memset( &layers, 0, sizeof( layers ) );
	switch ( r_vrKind ) {
	case VRF_GAME:
		layers.projection = vk_vr_eyes_written() ? VR_PROJ_FRESH : VR_PROJ_NONE;
		layers.dim = VR_GetComfortFade() > 0.0f;	// the head is in a wall
		layers.vignette = 1;						// shown while moving or turning (vr_comfortVignette)
		layers.quadMode = VR_QUAD_HUD;
		layers.placement.distance = vr_hudDistance->value;
		layers.placement.width = vr_hudWidth->value;
		layers.placement.height = vr_hudHeight->value;
		layers.placement.follow = vr_hudFollow->integer;
		layers.placement.followAngle = vr_hudFollowAngle->value;
		if ( vr_wheelOpen->integer ) {
			// the weapon wheel is drawn on the canvas: in front of the head where it opened, and
			// held there, so the hand can point at it
			layers.quadMode = VR_QUAD_WHEEL;
			layers.placement.distance = vr_wheelDistance->value;
			layers.placement.width = vr_wheelWidth->value;
			layers.placement.height = vr_wheelHeight->value;
			layers.placement.follow = 0;
		}
		break;
	case VRF_MENU:
	case VRF_MENU_LIVE:
		if ( r_vrHold ) {
			// still the HUD on the frozen world, as the last game frame left them
			layers.projection = VR_PROJ_FROZEN;
			layers.quadMode = VR_QUAD_HUD;
			layers.placement.distance = vr_hudDistance->value;
			layers.placement.width = vr_hudWidth->value;
			layers.placement.height = vr_hudHeight->value;
			layers.placement.follow = vr_hudFollow->integer;
			layers.placement.followAngle = vr_hudFollowAngle->value;
			break;
		}
		layers.projection = r_vrKind == VRF_MENU_LIVE && vk_vr_eyes_written() ? VR_PROJ_FRESH : VR_PROJ_FROZEN;
		layers.dim = vr_menuDim->value > 0.0f;
		layers.quadMode = VR_QUAD_MENU;
		layers.placement.distance = vr_menuDistance->value;
		layers.placement.width = vr_menuWidth->value;
		layers.placement.follow = vr_menuHeadLocked->integer ? 1 : vr_menuFollow->integer ? VR_QUAD_FOLLOW_SOFT : 0;
		layers.placement.followLag = vr_menuFollowLag->value;
		layers.laser = vr_menuLaser->integer;
		break;
	case VRF_THEATER:
		layers.projection = VR_PROJ_FROZEN;
		layers.dim = vr_theaterDim->value > 0.0f;
		layers.quadMode = VR_QUAD_THEATER;
		layers.quadOpaque = 1;
		layers.placement.distance = vr_theaterDistance->value;
		layers.placement.width = vr_theaterWidth->value;
		layers.placement.follow = 0;
		break;
	}
	layers.quad = r_vrHold ? 1 : vk_vr_canvas_written();
	layers.debug = vr_debugFrames->integer;

	// single-threaded back end: the frame's command buffer has been submitted by now
	VR_EndFrame( &layers );
}

/*
How RE_RenderScene draws this scene: 2 = once per eye, into the eye images; 0 = once, flat,
into the 2D canvas; -1 = not at all.
*/
int R_VR_SceneEyes( const refdef_t *fd ) {
	if ( !r_vrFrame || !VR_FrameActive() ) {
		return 0;
	}
	if ( fd->rdflags & RDF_NOWORLDMODEL ) {
		return 0;	// HUD heads, menu models: 2D canvas content
	}
	switch ( r_vrKind ) {
	case VRF_MENU:
		if ( r_vrHold && ( backEnd.screenshotMask & SCREENSHOT_SAVETHUMB ) ) {
			// the save thumbnail's frame: the world flat into the canvas, unseen (see VR_THUMB_WAIT)
			r_vrThumbWait = 0;
			return 0;
		}
		return -1;	// the last world frame stays up, frozen and dimmed
	case VRF_THEATER:
		return 0;	// the camera's view is the picture on the cutscene screen
	default:
		break;
	}
	if ( VR_GetEyeImageIndex( 0 ) < 0 || VR_GetEyeImageIndex( 1 ) < 0 ) {
		return 0;
	}
	return VR_MAX_EYES;
}

// OpenXR (+X right, +Y up, -Z forward) to the game's head frame (+X forward, +Y left, +Z up)
static void R_VR_FromXr( const float *xr, vec3_t out ) {
	out[0] = -xr[2];
	out[1] = -xr[0];
	out[2] = xr[1];
}

// head frame -> world, turned by the body's yaw
static void R_VR_ToWorld( const vec3_t head, const vec3_t fwd, const vec3_t left, vec3_t out ) {
	out[0] = fwd[0] * head[0] + left[0] * head[1];
	out[1] = fwd[1] * head[0] + left[1] * head[1];
	out[2] = head[2];
}

/*
R_VR_SetupEye

parms = base (the game's view), re-aimed for one eye:
- body yaw comes from the game's view; pitch and roll come from the headset only
- origin = the game's eye + the eye's offset from the recentred head origin, in game units
- viewport = the whole eye image
- projection from the eye's asymmetric field of view (R_SetupProjection, vrEye)
*/
void R_VR_SetupEye( viewParms_t *parms, const viewParms_t *base, int eye ) {
	vrEyeView_t view;
	vrSwapchainInfo_t info;
	float offsetXr[3], col[3][3], v[3];
	float qx, qy, qz, qw, yaw, scale;
	vec3_t fwd, left, head, world;

	*parms = *base;

	if ( !VR_GetEyeView( eye, &view ) || !VR_GetEyeSwapchain( eye, &info ) ) {
		return;
	}
	VR_GetEyeOffset( eye, offsetXr );

	// body yaw; when the game looks straight up or down, the view's up vector gives it. In
	// Holomatch H1 the view follows the gun's aim, and the client says what the body's yaw is.
	if ( VR_GetBodyYaw( &yaw ) ) {
		yaw = DEG2RAD( yaw );
	} else if ( fabs( base->or.axis[0][0] ) + fabs( base->or.axis[0][1] ) > 0.001f ) {
		yaw = atan2( base->or.axis[0][1], base->or.axis[0][0] );
	} else if ( base->or.axis[0][2] < 0 ) {
		yaw = atan2( base->or.axis[2][1], base->or.axis[2][0] );
	} else {
		yaw = atan2( -base->or.axis[2][1], -base->or.axis[2][0] );
	}
	VectorSet( fwd, cos( yaw ), sin( yaw ), 0 );
	VectorSet( left, -sin( yaw ), cos( yaw ), 0 );

	// the eye's rotation matrix (columns: its right, up and back axes in OpenXR space)
	qx = view.orientation[0]; qy = view.orientation[1]; qz = view.orientation[2]; qw = view.orientation[3];
	col[0][0] = 1 - 2 * ( qy * qy + qz * qz ); col[0][1] = 2 * ( qx * qy + qw * qz ); col[0][2] = 2 * ( qx * qz - qw * qy );
	col[1][0] = 2 * ( qx * qy - qw * qz ); col[1][1] = 1 - 2 * ( qx * qx + qz * qz ); col[1][2] = 2 * ( qy * qz + qw * qx );
	col[2][0] = 2 * ( qx * qz + qw * qy ); col[2][1] = 2 * ( qy * qz - qw * qx ); col[2][2] = 1 - 2 * ( qx * qx + qy * qy );

	// forward = -back
	v[0] = -col[2][0]; v[1] = -col[2][1]; v[2] = -col[2][2];
	R_VR_FromXr( v, head );
	R_VR_ToWorld( head, fwd, left, parms->or.axis[0] );
	// left = -right
	v[0] = -col[0][0]; v[1] = -col[0][1]; v[2] = -col[0][2];
	R_VR_FromXr( v, head );
	R_VR_ToWorld( head, fwd, left, parms->or.axis[1] );
	// up
	R_VR_FromXr( col[1], head );
	R_VR_ToWorld( head, fwd, left, parms->or.axis[2] );

	scale = vr_worldScale ? vr_worldScale->value : 32.0f;
	R_VR_FromXr( offsetXr, head );
	R_VR_ToWorld( head, fwd, left, world );
	VectorMA( base->or.origin, scale, world, parms->or.origin );
	VectorCopy( parms->or.origin, parms->pvsOrigin );

	// the whole eye image (the backend draws eye views at the eye image's size)
	parms->viewportX = 0;
	parms->viewportY = 0;
	parms->viewportWidth = (int)info.width;
	parms->viewportHeight = (int)info.height;
	parms->scissorX = 0;
	parms->scissorY = 0;
	parms->scissorWidth = parms->viewportWidth;
	parms->scissorHeight = parms->viewportHeight;

	parms->vrEye = eye + 1;
	parms->vrTan[0] = tan( view.angleLeft );
	parms->vrTan[1] = tan( view.angleRight );
	parms->vrTan[2] = tan( view.angleUp );
	parms->vrTan[3] = tan( view.angleDown );
}

#endif // EF_VR
