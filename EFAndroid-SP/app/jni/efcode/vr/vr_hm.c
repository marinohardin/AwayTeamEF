/*
===========================================================================
AwayTeamEF VR: Holomatch H1, when it is active and the cgame cvars it sets (EF_VR only).

This file is part of AwayTeamEF and is licensed under the GNU General
Public License, version 2 or (at your option) any later version.
===========================================================================

Compiled into the engine target (an INTERFACE source of ef_vr). docs/vr/HOLOMATCH-H1.md, §2
and §4.4-4.5.

While H1 is active the engine sets a few of the Holomatch cgame's own archived cvars:
  cg_bobup, cg_bobpitch, cg_bobroll   0: the view bob would move the eye the aim starts from,
                                      and the head already moves the view
  cg_drawCrosshair                    0: the screen-centre crosshair points nowhere in VR; the
                                      laser and the dot replace it
The player's own values are saved first and put back when H1 ends (leaving VR or Holomatch, a
disconnect, shutdown). The config file never sees the forced values: while it is written,
VR_HM_ArchiveValue hands cvar.c the saved ones, so a crash can't leave a flat profile changed.
A cvar the running cgame protects (cheat, read-only) is left alone. A value the player changes
by hand while H1 runs is theirs: it is kept, saved and no longer forced.
*/

#include "../client/client.h"
#include "vr.h"
#include "vr_hm.h"

extern int	SP_IsActive( void );	// sp_bridge.cpp: a single-player game is running

// the Holomatch player state's values (the game's protocol, which every client shares)
#define HM_PM_SPECTATOR		2
#define HM_PM_DEAD			3
#define HM_PM_INTERMISSION	5
#define HM_PMF_FOLLOW		4096
#define HM_STAT_HEALTH		0

#define VR_HM_MAX_VALUE		64

typedef struct {
	const char	*name;
	const char	*value;			// what H1 sets
	qboolean	forced;			// set by us, the saved value is the player's
	qboolean	existed;		// the cvar existed before (else the default is put back)
	char		saved[VR_HM_MAX_VALUE];
} vrHmCvar_t;

static vrHmCvar_t vr_hmCvars[] = {
	{ "cg_bobup",			"0" },
	{ "cg_bobpitch",		"0" },
	{ "cg_bobroll",			"0" },
	{ "cg_drawCrosshair",	"0" },
};
#define VR_HM_NUM_CVARS ( sizeof( vr_hmCvars ) / sizeof( vr_hmCvars[0] ) )

static qboolean	vr_hmApplied;
static qboolean	vr_hmWasActive;
static cvar_t	*vr_menuLiveWorld;
static cvar_t	*vr_hmTheater;

int VR_HM_Active( void ) {
	if ( !Com_Holomatch() || !cgvm || clc.state != CA_ACTIVE || clc.demoplaying ) {
		return 0;
	}
	if ( SP_IsActive() || !Cvar_VariableIntegerValue( "vr_active" ) ) {
		return 0;
	}
	return 1;
}

vrHmState_t VR_HM_State( void ) {
	const playerState_t *ps = &cl.snap.ps;

	if ( ps->pm_type == HM_PM_INTERMISSION ) {
		return VR_HM_INTERMISSION;
	}
	if ( ps->pm_flags & HM_PMF_FOLLOW ) {
		return VR_HM_FOLLOW;
	}
	if ( ps->pm_type == HM_PM_SPECTATOR ) {
		return VR_HM_SPECTATOR;
	}
	if ( ps->pm_type == HM_PM_DEAD || ps->stats[HM_STAT_HEALTH] <= 0 ) {
		return VR_HM_DEAD;
	}
	return VR_HM_PLAY;
}

static qboolean VR_HM_Protected( int flags ) {
	if ( flags & ( CVAR_ROM | CVAR_INIT | CVAR_PROTECTED ) ) {
		return qtrue;
	}
	if ( ( flags & CVAR_CHEAT ) && !Cvar_VariableIntegerValue( "sv_cheats" ) ) {
		return qtrue;
	}
	return qfalse;
}

static void VR_HM_Apply( void ) {
	unsigned i;

	for ( i = 0; i < VR_HM_NUM_CVARS; i++ ) {
		vrHmCvar_t *c = &vr_hmCvars[i];
		const int flags = Cvar_Flags( c->name );

		c->existed = ( flags & CVAR_NONEXISTENT ) ? qfalse : qtrue;
		if ( c->existed && VR_HM_Protected( flags ) ) {
			Com_Printf( "VR Holomatch: %s is protected by the cgame, left as it is\n", c->name );
			continue;
		}
		Q_strncpyz( c->saved, c->existed ? Cvar_VariableString( c->name ) : "", sizeof( c->saved ) );
		Cvar_Set( c->name, c->value );
		c->forced = qtrue;
	}
	vr_hmApplied = qtrue;
}

static void VR_HM_RestoreCvars( void ) {
	unsigned i;

	if ( !vr_hmApplied ) {
		return;
	}
	for ( i = 0; i < VR_HM_NUM_CVARS; i++ ) {
		vrHmCvar_t *c = &vr_hmCvars[i];

		if ( !c->forced ) {
			continue;
		}
		c->forced = qfalse;
		if ( c->existed ) {
			Cvar_Set( c->name, c->saved );
		} else {
			Cvar_Reset( c->name );	// the cgame's default, once it has registered it
		}
	}
	vr_hmApplied = qfalse;
}

// a forced value the player has changed since is theirs now
static void VR_HM_CheckChanged( void ) {
	unsigned i;

	for ( i = 0; i < VR_HM_NUM_CVARS; i++ ) {
		vrHmCvar_t *c = &vr_hmCvars[i];

		if ( c->forced && strcmp( Cvar_VariableString( c->name ), c->value ) ) {
			c->forced = qfalse;
		}
	}
}

void VR_HM_UpdateCvars( int active ) {
	if ( active && !vr_hmApplied ) {
		VR_HM_Apply();
	} else if ( !active && vr_hmApplied ) {
		VR_HM_RestoreCvars();
	} else if ( active ) {
		VR_HM_CheckChanged();
	}
	if ( active != vr_hmWasActive ) {
		vr_hmWasActive = active ? qtrue : qfalse;
		VR_HM_AimReset();
		Com_Printf( "VR Holomatch: %s\n", active ? "active (aim from the hand)" : "off" );
	}
}

void VR_HM_Restore( void ) {
	VR_HM_RestoreCvars();
	if ( vr_hmWasActive ) {
		vr_hmWasActive = qfalse;
		VR_HM_AimReset();
	}
}

int VR_HM_UiState( int menuUp ) {
	vrHmState_t st;

	if ( !vr_menuLiveWorld ) {
		vr_menuLiveWorld = Cvar_Get( "vr_menuLiveWorld", "1", CVAR_ARCHIVE );
		Cvar_CheckRange( vr_menuLiveWorld, 0, 1, qtrue );
		Cvar_SetDescription( vr_menuLiveWorld, "VR, Holomatch: behind a menu the match goes on, dimmed (1), or the world freezes (0)." );
		vr_hmTheater = Cvar_Get( "vr_hmTheater", "1", CVAR_ARCHIVE );
		Cvar_CheckRange( vr_hmTheater, 0, 1, qtrue );
		Cvar_SetDescription( vr_hmTheater, "VR, Holomatch: following another player and the intermission on a big screen (1), or in stereo (0)." );
	}
	if ( menuUp ) {
		return vr_menuLiveWorld->integer ? 3 : 0;
	}
	st = VR_HM_State();
	if ( ( st == VR_HM_FOLLOW || st == VR_HM_INTERMISSION ) && vr_hmTheater->integer ) {
		return 4;
	}
	return 0;
}

const char *VR_HM_ArchiveValue( const char *name, const char *resetString ) {
	unsigned i;

	if ( !vr_hmApplied ) {
		return NULL;
	}
	for ( i = 0; i < VR_HM_NUM_CVARS; i++ ) {
		const vrHmCvar_t *c = &vr_hmCvars[i];

		if ( c->forced && !Q_stricmp( name, c->name ) ) {
			return c->existed ? c->saved : ( resetString ? resetString : "" );
		}
	}
	return NULL;
}
