/*
===========================================================================
AwayTeamEF VR: calls between the files of the vr module (EF_VR only).

This file is part of AwayTeamEF and is licensed under the GNU General
Public License, version 2 or (at your option) any later version.
===========================================================================

Include after <openxr/openxr.h>. Nothing outside efcode/vr uses this.
*/
#ifndef EF_VR_LOCAL_H
#define EF_VR_LOCAL_H

#include "vr_input.h"

// vr_input.c, driven by vr_openxr.c:
//   VR_Init           -> VR_Input_Init( instance, xrGetInstanceProcAddr )   actions + bindings
//   VR_CreateSession  -> VR_Input_AttachSession( session )                  pose spaces, attach
//   VR_BeginFrame     -> VR_Input_Sync( space, displayTime, focused )       every frame
//   VR_DestroySession -> VR_Input_DetachSession()
//   VR_Shutdown       -> VR_Input_Shutdown()
//   interaction profile changed event -> VR_Input_ProfileChanged()
int			VR_Input_Init( XrInstance instance, PFN_xrGetInstanceProcAddr getInstanceProcAddr );
int			VR_Input_AttachSession( XrSession session );
void		VR_Input_Sync( XrSpace space, XrTime displayTime, int focused );
void		VR_Input_ProfileChanged( void );
void		VR_Input_DetachSession( void );
void		VR_Input_Shutdown( void );

#endif // EF_VR_LOCAL_H
