/*
===========================================================================
AwayTeamEF VR: Holomatch H1, VR on any Holomatch server (EF_VR only).

This file is part of AwayTeamEF and is licensed under the GNU General
Public License, version 2 or (at your option) any later version.
===========================================================================

docs/vr/HOLOMATCH-H1.md explains the design. In short: everything is in the engine, and only
while VR runs in Holomatch (VR_HM_Active). The server and the game modules see a normal player:
the usercmd, the protocol, the game modules, the paks and the userinfo are unchanged.

  vr_hm.c        VR_HM_Active, the cgame cvars forced while active (saved and put back; the
                 config file always gets the player's own values), the special states
  vr_hm_aim.c    the aim: the hand ray, the convergence onto its hit point, the body yaw apart
                 from the aim, the delta angles, the pitch clamp, roll 0; the laser and the
                 predicted-impact dot; the muzzle table
  vr_hm_trace.c  the engine's own trace against the world, the movers and the players of the
                 snapshots, at the positions the cgame draws

Who calls what (all under EF_VR):
  cl_input.c     VR_HM_Cmd, after CL_FinishMove: the command's angles and movement
  cl_cgame.c     VR_HM_Scene, before each scene the cgame renders: the view, the laser and dot
  cl_main.c      VR_HM_Restore, on a disconnect and at shutdown
  cvar.c         VR_HM_ArchiveValue, while the config is written
*/
#ifndef EF_VR_HM_H
#define EF_VR_HM_H

#ifdef __cplusplus
extern "C" {
#endif

// VR runs (vr_active), Holomatch (com_holomatch), in a game through the Holomatch cgame (not the
// single-player bridge), not a demo
int		VR_HM_Active( void );

// once per command, from CL_CreateCmd after CL_FinishMove: applies or puts back the forced cvars,
// and, while active, sets the command's angles to the aim and turns its movement into them
struct usercmd_s;
void	VR_HM_Cmd( struct usercmd_s *cmd );

// before the cgame's refdef (a refdef_t *) is rendered: records the world view, adds the laser
// and the dot
void	VR_HM_Scene( const void *refdef );

// put the player's own cgame cvars back (a disconnect, shutdown)
void	VR_HM_Restore( void );

// while the config is written: the player's own value of a cvar H1 forces (resetString: the
// cvar's default, for one that didn't exist before), else NULL
const char *VR_HM_ArchiveValue( const char *name, const char *resetString );

// ---- vr_hm.c, for the other vr_hm files ----
typedef enum {
	VR_HM_PLAY,			// alive, in play: the hand aims
	VR_HM_DEAD,			// the body yaw and the angles are frozen
	VR_HM_SPECTATOR,	// free spectator: the angles follow the head
	VR_HM_FOLLOW,		// following another player: nothing is sent that matters
	VR_HM_INTERMISSION
} vrHmState_t;
vrHmState_t VR_HM_State( void );
void	VR_HM_UpdateCvars( int active );

// ---- vr_hm_trace.c ----
typedef struct {
	float	fraction;
	float	endpos[3];
	float	normal[3];
	int		entityNum;		// the snapshot entity hit, or -1 for the world (or nothing)
	int		startsolid;
} vrHmTrace_t;
// a point (or box, mins/maxs may be NULL) trace through the world, the movers and the players of
// the snapshots, interpolated to cl.serverTime as the cgame draws them; skip = entity number to
// pass through (the player's own)
void	VR_HM_Trace( vrHmTrace_t *out, const float start[3], const float end[3],
				const float *mins, const float *maxs, int skip, int mask );

// ---- vr_hm_aim.c, for vr_hm.c ----
void	VR_HM_AimReset( void );

#ifdef __cplusplus
}
#endif

#endif // EF_VR_HM_H
