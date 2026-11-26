/*
===========================================================================
AwayTeamEF VR: the renderer's side of the VR build (EF_VR only).

This file is part of AwayTeamEF and is licensed under the GNU General
Public License, version 2 or (at your option) any later version.
===========================================================================

- cvars vr_enable / vr_worldScale and the vr_recenter command
- per frame: VR_BeginFrame from RE_BeginFrame, VR_EndFrame from RE_EndFrame
- per world scene: the view is drawn once per eye (RE_RenderScene calls R_VR_SetupEye),
  each eye into its half of the frame, with the eye's pose and asymmetric field of view

Without EF_VR this file compiles to nothing.
*/

#ifdef EF_VR

#include "tr_local.h"
#include "../vr/vr.h"

cvar_t	*vr_enable;
cvar_t	*vr_worldScale;

static qboolean r_vrFrame;		// R_VR_BeginFrame ran VR_BeginFrame; R_VR_EndFrame is due
static qboolean r_vrWarnedSmp;

static void R_VR_Recenter_f( void ) {
	VR_Recenter();
}

void R_VR_Register( void ) {
#ifdef __ANDROID__
	const char *enableDefault = "1";	// the Quest build
#else
	const char *enableDefault = "0";	// a desktop VR build only uses a headset when asked
#endif

	vr_enable = ri.Cvar_Get( "vr_enable", enableDefault, CVAR_ARCHIVE_ND | CVAR_LATCH );
	ri.Cvar_CheckRange( vr_enable, "0", "1", CV_INTEGER );
	ri.Cvar_SetDescription( vr_enable, "Render to an OpenXR headset when one is present. Requires vid_restart." );

	vr_worldScale = ri.Cvar_Get( "vr_worldScale", "32", CVAR_ARCHIVE_ND );
	ri.Cvar_CheckRange( vr_worldScale, "8", "128", CV_FLOAT );
	ri.Cvar_SetDescription( vr_worldScale, "VR: game units per metre of head movement." );

	ri.Cmd_AddCommand( "vr_recenter", R_VR_Recenter_f );
}

void R_VR_Print( int isError, const char *msg ) {
	ri.Printf( isError ? PRINT_WARNING : PRINT_ALL, "%s", msg );
}

void R_VR_BeginFrame( void ) {
	r_vrFrame = qfalse;

	if ( !vk_vr_active() ) {
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

	VR_BeginFrame();
	r_vrFrame = qtrue;
}

void R_VR_EndFrame( void ) {
	if ( !r_vrFrame ) {
		return;
	}
	r_vrFrame = qfalse;

	// single-threaded back end: the frame's command buffer has been submitted by now
	VR_EndFrame( vk_vr_take_eyes_written() );
}

int R_VR_SceneEyes( const refdef_t *fd ) {
	if ( !r_vrFrame || !VR_FrameActive() ) {
		return 0;
	}
	if ( fd->rdflags & RDF_NOWORLDMODEL ) {
		return 0;	// HUD heads, menu models: 2D-layer content (docs/vr/RENDERING.md)
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
- viewport = the eye's half of the game's viewport
- projection from the eye's asymmetric field of view (R_SetupProjection, vrEye)
*/
void R_VR_SetupEye( viewParms_t *parms, const viewParms_t *base, int eye ) {
	vrEyeView_t view;
	float offsetXr[3], col[3][3], v[3];
	float qx, qy, qz, qw, yaw, scale;
	vec3_t fwd, left, head, world;
	int halfWidth;

	*parms = *base;

	if ( !VR_GetEyeView( eye, &view ) ) {
		return;
	}
	VR_GetEyeOffset( eye, offsetXr );

	// body yaw; when the game looks straight up or down, the view's up vector gives it
	if ( fabs( base->or.axis[0][0] ) + fabs( base->or.axis[0][1] ) > 0.001f ) {
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

	halfWidth = base->viewportWidth / 2;
	parms->viewportX = base->viewportX + eye * halfWidth;
	parms->viewportWidth = halfWidth;
	parms->scissorX = parms->viewportX;
	parms->scissorWidth = parms->viewportWidth;

	parms->vrEye = qtrue;
	parms->vrTan[0] = tan( view.angleLeft );
	parms->vrTan[1] = tan( view.angleRight );
	parms->vrTan[2] = tan( view.angleUp );
	parms->vrTan[3] = tan( view.angleDown );
}

#endif // EF_VR
