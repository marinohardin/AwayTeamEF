/*
===========================================================================
Launcher: key binding sets for the Controls buttons (recommended / the
game's original ones), applied to a mode's config file.

Plain C with no other dependency, so the macOS launcher compiles it too
(it doesn't use the rest of the core).

This file is part of VoyagerSP (GPLv2 or later, like the rest of the engine).
===========================================================================
*/

#ifndef LC_CONTROLS_H
#define LC_CONTROLS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	LC_CONTROLS_RECOMMENDED,	/* WASD + mouse, E use, Q mission analysis, F5 instant save, ... */
	LC_CONTROLS_ORIGINAL		/* the game's own: default.cfg's, from the game data */
} lc_controls_set;

/*
The engine runs default.cfg, then the mode's config (efspconfig.cfg or
hmconfig.cfg), which it writes as "unbindall" and every binding. So:
  - recommended: the config's bind lines are replaced by "unbindall" and the
    recommended set;
  - original: the config's bind lines are removed, so default.cfg's apply
    (and the game writes them back to the config when it quits).
Every other line is kept as it is.

text = the config's contents (NULL or "" for a missing file). Returns the new
contents (malloc'd, free() it; NULL only when out of memory).
*/
char *lc_controls_rewrite( const char *text, int holomatch, lc_controls_set set );

/* 1 when text binds exactly the recommended keys (in any order, key names in any case) */
int lc_controls_is_recommended( const char *text, int holomatch );

/* the config file names, relative to the home folder (baseEF) */
#define LC_CONTROLS_SP_CONFIG	"efspconfig.cfg"
#define LC_CONTROLS_HM_CONFIG	"hmconfig.cfg"

#ifdef __cplusplus
}
#endif

#endif
