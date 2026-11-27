/*
===========================================================================
AwayTeamEF VR: the weapon wheel (EF_VR only).

This file is part of AwayTeamEF and is licensed under the GNU General
Public License, version 2 or (at your option) any later version.
===========================================================================

Holding the wheel button (the right thumbstick click) opens a ring of the player's weapons on
the 2D layer, world-locked in front of the head. The weapon hand points at one (or the right
stick picks it) and letting go of the button switches to it with the game's own "weapon <n>"
command. In single player, and only there (no co-op session, not Holomatch), time slows while
the wheel is open. Details: docs/vr/QUEST.md.

  vr_input_engine.c   the button: tap = its old meaning, hold = VR_Wheel_Open, release =
                      VR_Wheel_Close; fire and turning are held off while it is open
  cl_scrn.c           VR_Wheel_Draw, after the cgame's HUD
  renderervk/tr_vr.c  puts the 2D quad at the wheel's place while vr_wheelOpen is 1
  sp/sp_bridge.cpp    the weapon list from the single-player cgame (CG_VR_SETWEAPONS), and
                      VR_Wheel_TimeScale on its simulation clock
*/
#ifndef EF_VR_WHEEL_H
#define EF_VR_WHEEL_H

#ifdef __cplusplus
extern "C" {
#endif

#define VR_WHEEL_MAX		16

// the same layout as vrWheelWeapons_t in efgame/src/game/vr_shared.h (the bridge copies it)
typedef struct {
	int		count;
	int		current;					// the weapon selected now
	int		locked;						// no switching now
	int		dead;						// the player is dead
	int		noAmmoIcon;					// shader drawn over a weapon that's out of ammo
	int		weapon[VR_WHEEL_MAX];
	int		icon[VR_WHEEL_MAX];			// shader handles
	int		noAmmo[VR_WHEEL_MAX];
	char	name[VR_WHEEL_MAX][32];
} vrWheelList_t;

int			VR_Wheel_Open( void );						// 1 = it opened (there is something to pick)
void		VR_Wheel_Close( int pick, const char *why );	// pick = switch to the highlighted weapon
int			VR_Wheel_IsOpen( void );
// every input frame (vr_input_engine.c): the pointer, the highlight and the reasons to close
void		VR_Wheel_Update( const float stick[2], int inPlay );
void		VR_Wheel_Draw( void );						// the client, in play, after the HUD
// the single-player clock's speed now: 1 unless the wheel is open in plain single player
float		VR_Wheel_TimeScale( void );
// a level ends, a game is saved or loaded, the client disconnects: shut at once, no pick
void		VR_Wheel_Reset( const char *why );

// sp_bridge.cpp: the single-player cgame's latest list; 0 when there is none, or it's stale
int			SP_VR_WheelWeapons( vrWheelList_t *out );

#ifdef __cplusplus
}
#endif

#endif // EF_VR_WHEEL_H
