/*
===========================================================================
Launcher: key binding sets for the Controls buttons (lc_controls.h).

This file is part of VoyagerSP (GPLv2 or later, like the rest of the engine).
===========================================================================
*/

#include "lc_controls.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *key, *command; } bind_t;

/* Shared by both modes: the rows of the game's Controls menus. Single player's
   Weapons / Attack-Look / Movement / Quick Keys pages and Holomatch's Weapons /
   Attack-Look / Movement / Reply pages. */
#define COMMON_BINDS \
	{ "TAB", "+info" },				/* Mission Info (SP) / Show Scores (HM) */ \
	{ "ENTER", "+button2" },		/* Skip Cinematic (SP) / Use Inventory (HM) */ \
	{ "SPACE", "+moveup" }, \
	{ ",", "+moveleft" }, \
	{ "-", "weapon 10" }, \
	{ ".", "+moveright" }, \
	{ "/", "+moveup" }, \
	{ "0", "weapon 0" }, \
	{ "1", "weapon 1" }, { "2", "weapon 2" }, { "3", "weapon 3" }, \
	{ "4", "weapon 4" }, { "5", "weapon 5" }, { "6", "weapon 6" }, \
	{ "7", "weapon 7" }, { "8", "weapon 8" }, { "9", "weapon 9" }, \
	{ "=", "weapon 11" }, \
	{ "[", "weapprev" }, \
	{ "\\", "+mlook" }, \
	{ "]", "weapnext" }, \
	{ "`", "toggleconsole" }, \
	{ "a", "+moveleft" }, \
	{ "c", "+movedown" }, \
	{ "d", "+moveright" }, \
	{ "e", "+use" }, \
	{ "s", "+back" }, \
	{ "t", "messagemode2" }, \
	{ "w", "+forward" }, \
	{ "x", "+moveup" }, \
	{ "y", "messagemode" }, \
	{ "z", "+zoom" }, \
	{ "~", "toggleconsole" }, \
	{ "UPARROW", "+forward" }, \
	{ "DOWNARROW", "+back" }, \
	{ "LEFTARROW", "+left" }, \
	{ "RIGHTARROW", "+right" }, \
	{ "ALT", "+altattack" }, \
	{ "CTRL", "+attack" }, \
	{ "SHIFT", "+speed" }, \
	{ "DEL", "+lookdown" }, \
	{ "PGDN", "+lookup" }, \
	{ "END", "centerview" }, \
	{ "F1", "vote yes" }, \
	{ "F2", "vote no" }, \
	{ "F4", "levelselect" },		/* Load Menu (SP) */ \
	{ "F11", "screenshot" }, \
	{ "MOUSE1", "+attack" }, \
	{ "MOUSE2", "+altattack" }, \
	{ "MOUSE3", "+zoom" }, \
	{ "MWHEELDOWN", "weapnext" }, \
	{ "MWHEELUP", "weapprev" }

static const bind_t spBinds[] = {
	COMMON_BINDS,
	{ "h", "weapon 12" },
	{ "q", "+analysis" },			/* Mission Analysis */
	{ "F3", "ui_teamOrders" },		/* Save Menu */
	{ "F5", "save quik*" },			/* Instant Save */
};

static const bind_t hmBinds[] = {
	COMMON_BINDS,
	{ "f", "+button3" },			/* Gesture */
	{ "g", "messagemode3" },		/* Chat - Target */
	{ "h", "messagemode4" },		/* Chat - Attacker */
	{ "F3", "ui_teamorders" },
};

#define COUNT( a )	( sizeof( a ) / sizeof( ( a )[0] ) )

static const bind_t *Binds( int holomatch, size_t *count )
{
	*count = holomatch ? COUNT( hmBinds ) : COUNT( spBinds );
	return holomatch ? hmBinds : spBinds;
}

/* ---- parsing ------------------------------------------------------------------ */

/* the next whitespace-separated or "quoted" token of *p, into out; 0 at the end */
static int Token( const char **p, char *out, size_t size )
{
	const char *s = *p;
	size_t n = 0;

	while ( *s == ' ' || *s == '\t' )
		s++;
	if ( !*s || *s == '\n' || *s == '\r' ) {
		*p = s;
		return 0;
	}
	if ( *s == '"' ) {
		s++;
		while ( *s && *s != '"' && *s != '\n' && *s != '\r' ) {
			if ( n + 1 < size )
				out[n++] = *s;
			s++;
		}
		if ( *s == '"' )
			s++;
	} else {
		while ( *s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r' ) {
			if ( n + 1 < size )
				out[n++] = *s;
			s++;
		}
	}
	out[n] = 0;
	*p = s;
	return 1;
}

static int StrEqualCI( const char *a, const char *b )
{
	for ( ; *a && *b; a++, b++ ) {
		if ( tolower( (unsigned char)*a ) != tolower( (unsigned char)*b ) )
			return 0;
	}
	return *a == *b;
}

/* the line starting at line is a key binding command (bind, unbind, unbindall) */
static int IsBindLine( const char *line )
{
	char cmd[16];

	if ( !Token( &line, cmd, sizeof( cmd ) ) )
		return 0;
	return StrEqualCI( cmd, "bind" ) || StrEqualCI( cmd, "unbind" ) || StrEqualCI( cmd, "unbindall" );
}

/* a game controller's key: PAD0_A, PAD0_LEFTTRIGGER, ... and the older JOY1-32 / AUX1-16 */
static int IsControllerKey( const char *key )
{
	static const char *const prefixes[] = { "PAD", "JOY", "AUX" };
	size_t i, k;

	for ( i = 0; i < sizeof( prefixes ) / sizeof( prefixes[0] ); i++ ) {
		for ( k = 0; prefixes[i][k] && tolower( (unsigned char)key[k] ) == tolower( (unsigned char)prefixes[i][k] ); k++ )
			;
		if ( !prefixes[i][k] )
			return 1;
	}
	return 0;
}

/* the line starting at line binds a controller key (the Steam Deck layout, the player's own):
   both sets keep those, they only set the keyboard and mouse */
static int IsControllerBind( const char *line )
{
	char cmd[16], key[64];

	return Token( &line, cmd, sizeof( cmd ) ) && StrEqualCI( cmd, "bind" )
		&& Token( &line, key, sizeof( key ) ) && IsControllerKey( key );
}

/* ---- API ---------------------------------------------------------------------- */

char *lc_controls_rewrite( const char *text, int holomatch, lc_controls_set set )
{
	size_t count, i, len = text ? strlen( text ) : 0, room;
	const bind_t *binds = Binds( holomatch, &count );
	const char *p = text ? text : "";
	char *out, *o, *pads, *po;

	room = len + 2 + 32;		/* the kept lines, a newline, "unbindall\n" */
	for ( i = 0; i < count; i++ )
		room += strlen( binds[i].key ) + strlen( binds[i].command ) + 16;
	out = malloc( room );
	pads = malloc( len + 2 );	/* the controller bindings, kept */
	if ( !out || !pads ) {
		free( out );
		free( pads );
		return NULL;
	}
	o = out;
	po = pads;

	/* every line but the bind lines, as it is; the controller's bindings set aside */
	while ( *p ) {
		const char *end = strchr( p, '\n' );
		size_t n = end ? (size_t)( end - p ) + 1 : strlen( p );
		char **dst = !IsBindLine( p ) ? &o : IsControllerBind( p ) ? &po : NULL;

		if ( dst ) {
			memcpy( *dst, p, n );
			*dst += n;
			if ( !end ) {
				*( *dst )++ = '\n';	/* a last line without one */
			}
		}
		p += n;
	}
	*po = 0;

	/* after "unbindall", or (original) with default.cfg's, which binds no controller */
	if ( set == LC_CONTROLS_RECOMMENDED )
		o += strlen( strcpy( o, "unbindall\n" ) );
	o += strlen( strcpy( o, pads ) );
	free( pads );
	if ( set == LC_CONTROLS_RECOMMENDED ) {
		for ( i = 0; i < count; i++ ) {
			/* the engine's own form (Key_WriteBindings) */
			o += strlen( strcpy( o, "bind " ) );
			o += strlen( strcpy( o, binds[i].key ) );
			o += strlen( strcpy( o, " \"" ) );
			o += strlen( strcpy( o, binds[i].command ) );
			o += strlen( strcpy( o, "\"\n" ) );
		}
	}
	*o = 0;
	return out;
}

int lc_controls_is_recommended( const char *text, int holomatch )
{
	size_t count, i, found = 0, seen = 0;
	const bind_t *binds = Binds( holomatch, &count );
	const char *p = text ? text : "";
	unsigned char *matched = calloc( count ? count : 1, 1 );

	if ( !matched )
		return 0;
	while ( *p ) {
		const char *end = strchr( p, '\n' ), *q = p;
		char cmd[16], key[64], command[256];

		if ( Token( &q, cmd, sizeof( cmd ) ) && StrEqualCI( cmd, "bind" )
			&& Token( &q, key, sizeof( key ) ) && Token( &q, command, sizeof( command ) )
			&& !IsControllerKey( key ) ) {		/* the controller's are the player's own */
			seen++;
			for ( i = 0; i < count; i++ ) {
				if ( StrEqualCI( key, binds[i].key ) ) {
					if ( !matched[i] && StrEqualCI( command, binds[i].command ) ) {
						matched[i] = 1;
						found++;
					}
					break;
				}
			}
		}
		p = end ? end + 1 : p + strlen( p );
	}
	free( matched );
	return found == count && seen == count;
}
