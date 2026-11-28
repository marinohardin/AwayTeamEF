/*
===========================================================================
AwayTeamEF VR: controller input through OpenXR actions (EF_VR only).

This file is part of AwayTeamEF and is licensed under the GNU General
Public License, version 2 or (at your option) any later version.
===========================================================================

One action set ("gameplay") with semantic actions, so a runtime's own rebinding UI shows
what each one does. Suggested bindings for two interaction profiles:

  /interaction_profiles/oculus/touch_controller  (Quest 2, Quest Pro and Quest 3 controllers
                                                  all offer it)
  /interaction_profiles/khr/simple_controller    (fallback: select, menu and the poses only)

Every frame (VR_Input_Sync, from VR_BeginFrame after xrWaitFrame) the actions are synced and
the head and hand poses located at the frame's predicted display time, in the same space as
the eye views. The result goes to the engine (VR_EngineInput, vr_input_engine.c), which owns
the controller map onto game commands.
*/

#include <stdio.h>
#include <string.h>

#include "vr.h"

#define XR_USE_GRAPHICS_API_VULKAN
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>

#include "vr_local.h"

void VR_Log( int isError, const char *fmt, ... );	// vr_openxr.c

// ---------------------------------------------------------------------------
// entry points (own copies, loaded from the same xrGetInstanceProcAddr)
// ---------------------------------------------------------------------------

#define XRI_FUNCS \
	XR_FN( xrStringToPath ) \
	XR_FN( xrPathToString ) \
	XR_FN( xrCreateActionSet ) \
	XR_FN( xrDestroyActionSet ) \
	XR_FN( xrCreateAction ) \
	XR_FN( xrSuggestInteractionProfileBindings ) \
	XR_FN( xrAttachSessionActionSets ) \
	XR_FN( xrGetCurrentInteractionProfile ) \
	XR_FN( xrSyncActions ) \
	XR_FN( xrGetActionStateBoolean ) \
	XR_FN( xrGetActionStateFloat ) \
	XR_FN( xrGetActionStateVector2f ) \
	XR_FN( xrApplyHapticFeedback ) \
	XR_FN( xrCreateActionSpace ) \
	XR_FN( xrCreateReferenceSpace ) \
	XR_FN( xrLocateSpace ) \
	XR_FN( xrDestroySpace )

#define XR_FN( name ) static PFN_##name i##name;
XRI_FUNCS
#undef XR_FN

// ---------------------------------------------------------------------------
// actions
// ---------------------------------------------------------------------------

typedef enum {
	ACT_MOVE,			// vector2: left stick
	ACT_TURN,			// vector2: right stick
	ACT_TRIGGER,		// float, per hand: triggers
	ACT_SQUEEZE,		// float, per hand: grips
	ACT_BUTTON_A,		// boolean ...
	ACT_BUTTON_B,
	ACT_BUTTON_X,
	ACT_BUTTON_Y,
	ACT_MENU,
	ACT_RSTICK,
	ACT_LSTICK,
	ACT_AIM,			// pose, per hand
	ACT_GRIP,			// pose, per hand
	ACT_HAPTIC,			// vibration output, per hand (the weapon wheel's tick)
	NUM_ACTIONS
} vrAction_t;

static const struct {
	const char		*name;			// [a-z0-9_-.]
	const char		*localized;		// shown in the runtime's binding UI
	XrActionType	type;
	int				perHand;		// subaction paths /user/hand/left and /right
	unsigned		button;			// VR_BUTTON_* for boolean actions
} vr_actionDefs[NUM_ACTIONS] = {
	{ "move",		"Move",					XR_ACTION_TYPE_VECTOR2F_INPUT,	0, 0 },
	{ "turn",		"Turn",					XR_ACTION_TYPE_VECTOR2F_INPUT,	0, 0 },
	{ "trigger",	"Fire or jump",			XR_ACTION_TYPE_FLOAT_INPUT,		1, 0 },
	{ "squeeze",	"Alternate fire or crouch",	XR_ACTION_TYPE_FLOAT_INPUT,	1, 0 },
	{ "button_a",	"Use (A)",	XR_ACTION_TYPE_BOOLEAN_INPUT,	0, VR_BUTTON_A },
	{ "button_b",	"Weapon wheel (B)",		XR_ACTION_TYPE_BOOLEAN_INPUT,	0, VR_BUTTON_B },
	{ "button_x",	"Skip cutscene or use item (X)",	XR_ACTION_TYPE_BOOLEAN_INPUT,	0, VR_BUTTON_X },
	{ "button_y",	"Mission objectives",			XR_ACTION_TYPE_BOOLEAN_INPUT,	0, VR_BUTTON_Y },
	{ "menu",		"Menu",					XR_ACTION_TYPE_BOOLEAN_INPUT,	0, VR_BUTTON_MENU },
	{ "rstick",		"Weapon wheel (stick click)",	XR_ACTION_TYPE_BOOLEAN_INPUT,	0, VR_BUTTON_RSTICK },
	{ "lstick",		"Recentre",				XR_ACTION_TYPE_BOOLEAN_INPUT,	0, VR_BUTTON_LSTICK },
	{ "aim",		"Aim pose",				XR_ACTION_TYPE_POSE_INPUT,		1, 0 },
	{ "grip",		"Grip pose",			XR_ACTION_TYPE_POSE_INPUT,		1, 0 },
	{ "haptic",		"Vibration",			XR_ACTION_TYPE_VIBRATION_OUTPUT,	1, 0 },
};

typedef struct {
	vrAction_t	action;
	const char	*path;
} vrBinding_t;

#define TOUCH_PROFILE	"/interaction_profiles/oculus/touch_controller"
#define SIMPLE_PROFILE	"/interaction_profiles/khr/simple_controller"
#define L	"/user/hand/left/input/"
#define R	"/user/hand/right/input/"

static const vrBinding_t vr_touchBindings[] = {
	{ ACT_MOVE,			L "thumbstick" },
	{ ACT_TURN,			R "thumbstick" },
	{ ACT_TRIGGER,		L "trigger/value" },
	{ ACT_TRIGGER,		R "trigger/value" },
	{ ACT_SQUEEZE,		L "squeeze/value" },
	{ ACT_SQUEEZE,		R "squeeze/value" },
	{ ACT_BUTTON_A,		R "a/click" },
	{ ACT_BUTTON_B,		R "b/click" },
	{ ACT_BUTTON_X,		L "x/click" },
	{ ACT_BUTTON_Y,		L "y/click" },
	{ ACT_MENU,			L "menu/click" },
	{ ACT_RSTICK,		R "thumbstick/click" },
	{ ACT_LSTICK,		L "thumbstick/click" },
	{ ACT_AIM,			L "aim/pose" },
	{ ACT_AIM,			R "aim/pose" },
	{ ACT_GRIP,			L "grip/pose" },
	{ ACT_GRIP,			R "grip/pose" },
	{ ACT_HAPTIC,		"/user/hand/left/output/haptic" },
	{ ACT_HAPTIC,		"/user/hand/right/output/haptic" },
};

// no sticks: fire, use and the menu, so menus and doors still work
static const vrBinding_t vr_simpleBindings[] = {
	{ ACT_TRIGGER,		R "select/click" },
	{ ACT_BUTTON_X,		L "select/click" },
	{ ACT_MENU,			L "menu/click" },
	{ ACT_MENU,			R "menu/click" },
	{ ACT_AIM,			L "aim/pose" },
	{ ACT_AIM,			R "aim/pose" },
	{ ACT_GRIP,			L "grip/pose" },
	{ ACT_GRIP,			R "grip/pose" },
	{ ACT_HAPTIC,		"/user/hand/left/output/haptic" },
	{ ACT_HAPTIC,		"/user/hand/right/output/haptic" },
};

#undef L
#undef R

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------

static struct {
	XrInstance		instance;
	XrActionSet		actionSet;
	XrAction		actions[NUM_ACTIONS];
	XrPath			handPaths[VR_HANDS];

	XrSession		session;		// attached to
	XrSpace			aimSpaces[VR_HANDS];
	XrSpace			gripSpaces[VR_HANDS];
	XrSpace			viewSpace;

	int				profileDirty;
	char			profiles[VR_HANDS][XR_MAX_PATH_LENGTH];

	vrInputState_t	state;
} vri;

const vrInputState_t *VR_GetInputState( void ) {
	return &vri.state;
}

int VR_GetHandPose( int hand, vrPose_t *aim, vrPose_t *grip ) {
	if ( hand < 0 || hand >= VR_HANDS ) {
		return 0;
	}
	if ( aim ) {
		*aim = vri.state.aim[hand];
	}
	if ( grip ) {
		*grip = vri.state.grip[hand];
	}
	return vri.state.aim[hand].valid;
}

void VR_Haptic( int hand, float amplitude, float seconds ) {
	XrHapticActionInfo hai;
	XrHapticVibration vib;

	if ( hand < 0 || hand >= VR_HANDS || vri.session == XR_NULL_HANDLE || !ixrApplyHapticFeedback ) {
		return;
	}
	memset( &hai, 0, sizeof( hai ) );
	hai.type = XR_TYPE_HAPTIC_ACTION_INFO;
	hai.action = vri.actions[ACT_HAPTIC];
	hai.subactionPath = vri.handPaths[hand];
	memset( &vib, 0, sizeof( vib ) );
	vib.type = XR_TYPE_HAPTIC_VIBRATION;
	vib.duration = (XrDuration)( seconds * 1e9f );
	vib.frequency = XR_FREQUENCY_UNSPECIFIED;
	vib.amplitude = amplitude;
	ixrApplyHapticFeedback( vri.session, &hai, (const XrHapticBaseHeader *)&vib );
}

const char *VR_GetInteractionProfile( int hand ) {
	if ( hand < 0 || hand >= VR_HANDS ) {
		return "";
	}
	return vri.profiles[hand];
}

// ---------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------

static XrPath VR_Input_Path( const char *s ) {
	XrPath p = XR_NULL_PATH;
	if ( XR_FAILED( ixrStringToPath( vri.instance, s, &p ) ) ) {
		VR_Log( 1, "VR input: bad path %s\n", s );
		return XR_NULL_PATH;
	}
	return p;
}

static int VR_Input_Suggest( const char *profile, const vrBinding_t *bindings, int count ) {
	XrActionSuggestedBinding sb[32];
	XrInteractionProfileSuggestedBinding ipsb;
	XrResult res;
	int i, n = 0;

	for ( i = 0; i < count && n < (int)( sizeof( sb ) / sizeof( sb[0] ) ); i++ ) {
		XrPath p = VR_Input_Path( bindings[i].path );
		if ( p == XR_NULL_PATH ) {
			continue;
		}
		sb[n].action = vri.actions[bindings[i].action];
		sb[n].binding = p;
		n++;
	}

	memset( &ipsb, 0, sizeof( ipsb ) );
	ipsb.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
	ipsb.interactionProfile = VR_Input_Path( profile );
	ipsb.countSuggestedBindings = (uint32_t)n;
	ipsb.suggestedBindings = sb;
	res = ixrSuggestInteractionProfileBindings( vri.instance, &ipsb );
	if ( XR_FAILED( res ) ) {
		VR_Log( 0, "VR input: bindings for %s refused (%d)\n", profile, (int)res );
		return 0;
	}
	return 1;
}

int VR_Input_Init( XrInstance instance, PFN_xrGetInstanceProcAddr getInstanceProcAddr ) {
	XrActionSetCreateInfo asci;
	int i, ok = 1, profiles = 0;

	VR_Input_Shutdown();
	vri.instance = instance;

#define XR_FN( name ) \
	if ( XR_FAILED( getInstanceProcAddr( instance, #name, (PFN_xrVoidFunction *)&i##name ) ) || !i##name ) { \
		VR_Log( 1, "VR input: missing OpenXR entry point %s\n", #name ); \
		ok = 0; \
	}
	XRI_FUNCS
#undef XR_FN
	if ( !ok ) {
		vri.instance = XR_NULL_HANDLE;
		return 0;
	}

	vri.handPaths[VR_HAND_LEFT] = VR_Input_Path( "/user/hand/left" );
	vri.handPaths[VR_HAND_RIGHT] = VR_Input_Path( "/user/hand/right" );

	memset( &asci, 0, sizeof( asci ) );
	asci.type = XR_TYPE_ACTION_SET_CREATE_INFO;
	strcpy( asci.actionSetName, "gameplay" );
	strcpy( asci.localizedActionSetName, "Gameplay" );
	asci.priority = 0;
	if ( XR_FAILED( ixrCreateActionSet( instance, &asci, &vri.actionSet ) ) ) {
		VR_Log( 1, "VR input: xrCreateActionSet failed\n" );
		vri.actionSet = XR_NULL_HANDLE;
		vri.instance = XR_NULL_HANDLE;
		return 0;
	}

	for ( i = 0; i < NUM_ACTIONS; i++ ) {
		XrActionCreateInfo aci;

		memset( &aci, 0, sizeof( aci ) );
		aci.type = XR_TYPE_ACTION_CREATE_INFO;
		aci.actionType = vr_actionDefs[i].type;
		strncpy( aci.actionName, vr_actionDefs[i].name, XR_MAX_ACTION_NAME_SIZE - 1 );
		strncpy( aci.localizedActionName, vr_actionDefs[i].localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1 );
		if ( vr_actionDefs[i].perHand ) {
			aci.countSubactionPaths = VR_HANDS;
			aci.subactionPaths = vri.handPaths;
		}
		if ( XR_FAILED( ixrCreateAction( vri.actionSet, &aci, &vri.actions[i] ) ) ) {
			VR_Log( 1, "VR input: xrCreateAction %s failed\n", vr_actionDefs[i].name );
			VR_Input_Shutdown();
			return 0;
		}
	}

	profiles += VR_Input_Suggest( TOUCH_PROFILE, vr_touchBindings,
		(int)( sizeof( vr_touchBindings ) / sizeof( vr_touchBindings[0] ) ) );
	profiles += VR_Input_Suggest( SIMPLE_PROFILE, vr_simpleBindings,
		(int)( sizeof( vr_simpleBindings ) / sizeof( vr_simpleBindings[0] ) ) );
	if ( !profiles ) {
		VR_Log( 1, "VR input: the runtime took no controller bindings\n" );
		VR_Input_Shutdown();
		return 0;
	}

	VR_Log( 0, "VR input: %d actions, %d controller profiles\n", NUM_ACTIONS, profiles );
	return 1;
}

static XrSpace VR_Input_ActionSpace( XrSession session, vrAction_t action, int hand ) {
	XrActionSpaceCreateInfo ci;
	XrSpace space = XR_NULL_HANDLE;

	memset( &ci, 0, sizeof( ci ) );
	ci.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
	ci.action = vri.actions[action];
	ci.subactionPath = vri.handPaths[hand];
	ci.poseInActionSpace.orientation.w = 1.0f;
	if ( XR_FAILED( ixrCreateActionSpace( session, &ci, &space ) ) ) {
		return XR_NULL_HANDLE;
	}
	return space;
}

int VR_Input_AttachSession( XrSession session ) {
	XrSessionActionSetsAttachInfo ai;
	XrReferenceSpaceCreateInfo rci;
	int hand;

	if ( vri.actionSet == XR_NULL_HANDLE ) {
		return 0;
	}
	VR_Input_DetachSession();

	memset( &ai, 0, sizeof( ai ) );
	ai.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
	ai.countActionSets = 1;
	ai.actionSets = &vri.actionSet;
	if ( XR_FAILED( ixrAttachSessionActionSets( session, &ai ) ) ) {
		VR_Log( 1, "VR input: xrAttachSessionActionSets failed, no controllers\n" );
		return 0;
	}
	vri.session = session;

	for ( hand = 0; hand < VR_HANDS; hand++ ) {
		vri.aimSpaces[hand] = VR_Input_ActionSpace( session, ACT_AIM, hand );
		vri.gripSpaces[hand] = VR_Input_ActionSpace( session, ACT_GRIP, hand );
	}

	memset( &rci, 0, sizeof( rci ) );
	rci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
	rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	rci.poseInReferenceSpace.orientation.w = 1.0f;
	if ( XR_FAILED( ixrCreateReferenceSpace( session, &rci, &vri.viewSpace ) ) ) {
		vri.viewSpace = XR_NULL_HANDLE;
	}

	vri.profileDirty = 1;
	return 1;
}

void VR_Input_DetachSession( void ) {
	int hand;

	for ( hand = 0; hand < VR_HANDS; hand++ ) {
		if ( vri.aimSpaces[hand] != XR_NULL_HANDLE ) {
			ixrDestroySpace( vri.aimSpaces[hand] );
		}
		if ( vri.gripSpaces[hand] != XR_NULL_HANDLE ) {
			ixrDestroySpace( vri.gripSpaces[hand] );
		}
		vri.aimSpaces[hand] = vri.gripSpaces[hand] = XR_NULL_HANDLE;
		vri.profiles[hand][0] = '\0';
	}
	if ( vri.viewSpace != XR_NULL_HANDLE ) {
		ixrDestroySpace( vri.viewSpace );
		vri.viewSpace = XR_NULL_HANDLE;
	}

	if ( vri.session != XR_NULL_HANDLE ) {
		int synced = vri.state.frame != 0;

		vri.session = XR_NULL_HANDLE;
		memset( &vri.state, 0, sizeof( vri.state ) );
		if ( synced ) {
			VR_EngineInput( &vri.state );	// let go of everything the engine still holds
		}
	}
}

void VR_Input_Shutdown( void ) {
	VR_Input_DetachSession();
	if ( vri.actionSet != XR_NULL_HANDLE && ixrDestroyActionSet ) {
		ixrDestroyActionSet( vri.actionSet );	// destroys its actions too
	}
	vri.actionSet = XR_NULL_HANDLE;
	memset( vri.actions, 0, sizeof( vri.actions ) );
	vri.instance = XR_NULL_HANDLE;
}

void VR_Input_ProfileChanged( void ) {
	vri.profileDirty = 1;
}

// ---------------------------------------------------------------------------
// per frame
// ---------------------------------------------------------------------------

static void VR_Input_UpdateProfiles( void ) {
	int hand;

	vri.profileDirty = 0;
	for ( hand = 0; hand < VR_HANDS; hand++ ) {
		XrInteractionProfileState ips;
		char before[XR_MAX_PATH_LENGTH];
		uint32_t len = 0;
		XrResult res;

		memset( &ips, 0, sizeof( ips ) );
		ips.type = XR_TYPE_INTERACTION_PROFILE_STATE;
		snprintf( before, sizeof( before ), "%s", vri.profiles[hand] );
		vri.profiles[hand][0] = '\0';
		res = ixrGetCurrentInteractionProfile( vri.session, vri.handPaths[hand], &ips );
		if ( XR_SUCCEEDED( res ) && ips.interactionProfile != XR_NULL_PATH &&
			XR_FAILED( ixrPathToString( vri.instance, ips.interactionProfile, XR_MAX_PATH_LENGTH, &len,
				vri.profiles[hand] ) ) ) {
			vri.profiles[hand][0] = '\0';
		}
		if ( strcmp( before, vri.profiles[hand] ) ) {
			VR_Log( 0, "VR input: %s hand uses %s\n", hand == VR_HAND_LEFT ? "left" : "right",
				vri.profiles[hand][0] ? vri.profiles[hand] : "no controller" );
		} else if ( !vri.profiles[hand][0] && XR_FAILED( res ) ) {
			VR_Log( 1, "VR input: %s hand: xrGetCurrentInteractionProfile failed (%d)\n",
				hand == VR_HAND_LEFT ? "left" : "right", (int)res );
		}
	}
}

static void VR_Input_Locate( XrSpace space, XrSpace base, XrTime time, vrPose_t *pose ) {
	XrSpaceLocation loc;
	const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;

	memset( pose, 0, sizeof( *pose ) );
	pose->orientation[3] = 1.0f;
	if ( space == XR_NULL_HANDLE || base == XR_NULL_HANDLE ) {
		return;
	}
	memset( &loc, 0, sizeof( loc ) );
	loc.type = XR_TYPE_SPACE_LOCATION;
	if ( XR_FAILED( ixrLocateSpace( space, base, time, &loc ) ) || ( loc.locationFlags & need ) != need ) {
		return;
	}
	pose->position[0] = loc.pose.position.x;
	pose->position[1] = loc.pose.position.y;
	pose->position[2] = loc.pose.position.z;
	pose->orientation[0] = loc.pose.orientation.x;
	pose->orientation[1] = loc.pose.orientation.y;
	pose->orientation[2] = loc.pose.orientation.z;
	pose->orientation[3] = loc.pose.orientation.w;
	pose->valid = 1;
}

static float VR_Input_Float( vrAction_t action, XrPath subaction ) {
	XrActionStateGetInfo gi;
	XrActionStateFloat st;

	memset( &gi, 0, sizeof( gi ) );
	gi.type = XR_TYPE_ACTION_STATE_GET_INFO;
	gi.action = vri.actions[action];
	gi.subactionPath = subaction;
	memset( &st, 0, sizeof( st ) );
	st.type = XR_TYPE_ACTION_STATE_FLOAT;
	if ( XR_FAILED( ixrGetActionStateFloat( vri.session, &gi, &st ) ) || !st.isActive ) {
		return 0.0f;
	}
	return st.currentState;
}

static void VR_Input_Vector2( vrAction_t action, float out[2] ) {
	XrActionStateGetInfo gi;
	XrActionStateVector2f st;

	out[0] = out[1] = 0.0f;
	memset( &gi, 0, sizeof( gi ) );
	gi.type = XR_TYPE_ACTION_STATE_GET_INFO;
	gi.action = vri.actions[action];
	memset( &st, 0, sizeof( st ) );
	st.type = XR_TYPE_ACTION_STATE_VECTOR2F;
	if ( XR_FAILED( ixrGetActionStateVector2f( vri.session, &gi, &st ) ) || !st.isActive ) {
		return;
	}
	out[0] = st.currentState.x;
	out[1] = st.currentState.y;
}

static int VR_Input_Bool( vrAction_t action ) {
	XrActionStateGetInfo gi;
	XrActionStateBoolean st;

	memset( &gi, 0, sizeof( gi ) );
	gi.type = XR_TYPE_ACTION_STATE_GET_INFO;
	gi.action = vri.actions[action];
	memset( &st, 0, sizeof( st ) );
	st.type = XR_TYPE_ACTION_STATE_BOOLEAN;
	if ( XR_FAILED( ixrGetActionStateBoolean( vri.session, &gi, &st ) ) || !st.isActive ) {
		return 0;
	}
	return st.currentState ? 1 : 0;
}

void VR_Input_Sync( XrSpace space, XrTime displayTime, int focused ) {
	vrInputState_t *s = &vri.state;
	unsigned frame = s->frame + 1;
	XrActiveActionSet aas;
	XrActionsSyncInfo si;
	XrResult res;
	int i, hand;

	memset( s, 0, sizeof( *s ) );
	s->frame = frame;

	if ( vri.session == XR_NULL_HANDLE ) {
		return;
	}

	if ( focused ) {
		memset( &aas, 0, sizeof( aas ) );
		aas.actionSet = vri.actionSet;
		aas.subactionPath = XR_NULL_PATH;
		memset( &si, 0, sizeof( si ) );
		si.type = XR_TYPE_ACTIONS_SYNC_INFO;
		si.countActiveActionSets = 1;
		si.activeActionSets = &aas;
		res = ixrSyncActions( vri.session, &si );
		// XR_SESSION_NOT_FOCUSED is a success code: every action then reads inactive
		if ( res == XR_SUCCESS ) {
			s->active = 1;
		}
	}

	if ( s->active ) {
		// a controller may wake after the profile event (or the runtime may not send one for it):
		// while a hand has no profile, ask again about once a second
		if ( ( !vri.profiles[VR_HAND_LEFT][0] || !vri.profiles[VR_HAND_RIGHT][0] ) && ( frame % 72 ) == 0 ) {
			vri.profileDirty = 1;
		}
		if ( vri.profileDirty ) {
			VR_Input_UpdateProfiles();
		}

		VR_Input_Vector2( ACT_MOVE, s->move );
		VR_Input_Vector2( ACT_TURN, s->turn );
		for ( hand = 0; hand < VR_HANDS; hand++ ) {
			s->trigger[hand] = VR_Input_Float( ACT_TRIGGER, vri.handPaths[hand] );
			s->squeeze[hand] = VR_Input_Float( ACT_SQUEEZE, vri.handPaths[hand] );
		}
		for ( i = 0; i < NUM_ACTIONS; i++ ) {
			if ( vr_actionDefs[i].button && VR_Input_Bool( (vrAction_t)i ) ) {
				s->buttons |= vr_actionDefs[i].button;
			}
		}
		// log each control once, the first time it's used: shows from a headset log which
		// controls reach the game
		{
			static unsigned seenButtons;
			static int seenMove, seenTurn;

			if ( !seenMove && ( s->move[0] * s->move[0] + s->move[1] * s->move[1] ) > 0.09f ) {
				seenMove = 1;
				VR_Log( 0, "VR input: first use of move\n" );
			}
			if ( !seenTurn && ( s->turn[0] * s->turn[0] + s->turn[1] * s->turn[1] ) > 0.09f ) {
				seenTurn = 1;
				VR_Log( 0, "VR input: first use of turn\n" );
			}
			for ( i = 0; i < NUM_ACTIONS; i++ ) {
				const unsigned b = vr_actionDefs[i].button;
				if ( b && ( s->buttons & b ) && !( seenButtons & b ) ) {
					seenButtons |= b;
					VR_Log( 0, "VR input: first use of %s\n", vr_actionDefs[i].name );
				}
			}
		}
	}

	// poses track even while a system overlay has focus; the hands just don't act
	if ( displayTime > 0 ) {
		VR_Input_Locate( vri.viewSpace, space, displayTime, &s->head );
		for ( hand = 0; hand < VR_HANDS; hand++ ) {
			VR_Input_Locate( vri.aimSpaces[hand], space, displayTime, &s->aim[hand] );
			VR_Input_Locate( vri.gripSpaces[hand], space, displayTime, &s->grip[hand] );
		}
	}

	VR_EngineInput( s );
}
