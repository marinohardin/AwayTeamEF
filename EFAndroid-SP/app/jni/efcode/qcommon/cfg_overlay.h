/*
cfg_overlay: the player's own Holomatch config over a mod folder's.

Every game folder keeps its own hmconfig.cfg: joining a server that runs a mod
(fs_game tw2, say) restarts the game into that folder and execs its copy, a
snapshot of the player's keys and graphics from the first time they played
there. So the main config (baseEF/hmconfig.cfg) is applied on top:

- keys: every key bound there wins; a key it leaves unbound keeps the mod's
  binding, so a mod's own commands still have their keys
- engine settings (cvars the engine registers: graphics, sound, input, network,
  name): the main config's values; the mod's own settings (cvars its cgame, ui
  or game registers) keep the mod folder's values

While a mod is loaded the game saves both files: the mod's as before, and the
keys and engine settings changed there into the main config, so they stay
changed after leaving.

This file is the engine-free part (parsing, the cvar rule, the merge), so it
can be tested alone: tests/engine. The engine side is Com_HmConfigOverlay in
qcommon/common.c, with the help of cvar.c (values for cvars not registered yet)
and cl_keys.c (which keys changed).
*/
#ifndef CFG_OVERLAY_H
#define CFG_OVERLAY_H

#include <stddef.h>

#define CFGOV_MAX_NAME	256
#define CFGOV_MAX_VALUE	1024

typedef enum {
	CFGOV_NONE,			// blank or comment
	CFGOV_BIND,			// bind <key> <command>
	CFGOV_UNBINDALL,
	CFGOV_SET,			// set / seta <name> <value>
	CFGOV_OTHER			// anything else
} cfgovKind_t;

typedef struct {
	cfgovKind_t	kind;
	int			archive;					// seta (CFGOV_SET)
	char		name[CFGOV_MAX_NAME];		// the key (bind) or cvar name (set)
	char		value[CFGOV_MAX_VALUE];		// the command (bind) or value (set)
} cfgovLine_t;

// One line of a config (no newline needed); the kind, and *out filled in.
// Tokens are as the engine writes them: bare or in double quotes.
cfgovKind_t CfgOv_ParseLine( const char *line, size_t len, cfgovLine_t *out );

typedef void ( *cfgovLineFn )( const cfgovLine_t *line, const char *raw, size_t rawLen, void *ctx );
void CfgOv_ForEachLine( const char *text, cfgovLineFn fn, void *ctx );

// What the main config's value for a cvar does over a mod folder's
typedef enum {
	CFGOV_APPLY_NOW,		// an engine cvar: set it now
	CFGOV_APPLY_ON_REGISTER,// unknown yet: set it when the engine registers it,
							// drop it if a game module does
	CFGOV_KEEP_MOD			// a game module's: the mod's value stays
} cfgovSetAction_t;

cfgovSetAction_t CfgOv_SetAction( int exists, int userCreated, int vmCreated );

typedef struct {
	const char *key;		// as Key_KeynumToString gives it
	const char *binding;	// "" = unbound
} cfgovBind_t;

typedef struct {
	const char *name;
	const char *value;
} cfgovCvar_t;

// The main config with the keys changed in a mod and the engine cvars' current
// values put in. Bindings: the main config's, each changed key replaced (or
// removed when unbound, added when new). Cvars: each of the main config's lines,
// with an engine cvar's value where one is given; engine cvars it lacks are
// appended (seta). Other commands are kept. Returns malloc'd text.
char *CfgOv_MergeBase( const char *baseText,
	const cfgovBind_t *changed, int numChanged,
	const cfgovCvar_t *engine, int numEngine );

#endif
