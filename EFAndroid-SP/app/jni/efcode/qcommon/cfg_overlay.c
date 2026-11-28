/*
cfg_overlay: the player's own Holomatch config over a mod folder's (see cfg_overlay.h).
Engine-free: libc only, so tests/engine builds it alone.
*/
#include "cfg_overlay.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int StrIEq( const char *a, const char *b )
{
	while ( *a && *b ) {
		if ( tolower( (unsigned char)*a ) != tolower( (unsigned char)*b ) )
			return 0;
		a++;
		b++;
	}
	return *a == *b;
}

// The next token of [*p, end): bare, or between double quotes. 0 at the end of the line.
static int NextToken( const char **p, const char *end, char *out, size_t size )
{
	const char *s = *p, *start;
	size_t n;

	while ( s < end && ( *s == ' ' || *s == '\t' ) )
		s++;
	if ( s >= end ) {
		*p = s;
		return 0;
	}
	if ( *s == '"' ) {
		start = ++s;
		while ( s < end && *s != '"' )
			s++;
		n = (size_t)( s - start );
		if ( s < end )
			s++;		// the closing quote
	} else {
		start = s;
		while ( s < end && *s != ' ' && *s != '\t' )
			s++;
		n = (size_t)( s - start );
	}
	if ( n >= size )
		n = size - 1;
	memcpy( out, start, n );
	out[n] = 0;
	*p = s;
	return 1;
}

// The rest of the line's tokens, joined by single spaces (as the engine's bind and set do)
static void RestOfLine( const char **p, const char *end, char *out, size_t size )
{
	char tok[CFGOV_MAX_VALUE];
	size_t len = 0;

	out[0] = 0;
	while ( NextToken( p, end, tok, sizeof( tok ) ) ) {
		size_t t = strlen( tok );
		if ( len && len + 1 < size )
			out[len++] = ' ';
		if ( len + t >= size )
			t = size - 1 - len;
		memcpy( out + len, tok, t );
		len += t;
		out[len] = 0;
	}
}

cfgovKind_t CfgOv_ParseLine( const char *line, size_t len, cfgovLine_t *out )
{
	const char *p = line, *end = line + len;
	char cmd[32];

	memset( out, 0, sizeof( *out ) );
	while ( end > p && ( end[-1] == '\r' || end[-1] == '\n' ) )
		end--;
	{
		const char *s = p;
		while ( s < end && ( *s == ' ' || *s == '\t' ) )
			s++;
		if ( s >= end || ( end - s >= 2 && s[0] == '/' && s[1] == '/' ) )
			return out->kind = CFGOV_NONE;
	}
	if ( !NextToken( &p, end, cmd, sizeof( cmd ) ) )
		return out->kind = CFGOV_NONE;

	if ( StrIEq( cmd, "unbindall" ) )
		return out->kind = CFGOV_UNBINDALL;
	if ( StrIEq( cmd, "bind" ) ) {
		if ( !NextToken( &p, end, out->name, sizeof( out->name ) ) )
			return out->kind = CFGOV_OTHER;
		RestOfLine( &p, end, out->value, sizeof( out->value ) );
		if ( !out->value[0] )
			return out->kind = CFGOV_OTHER;		// "bind key" only prints it
		return out->kind = CFGOV_BIND;
	}
	if ( StrIEq( cmd, "set" ) || StrIEq( cmd, "seta" ) ) {
		if ( !NextToken( &p, end, out->name, sizeof( out->name ) ) )
			return out->kind = CFGOV_OTHER;
		out->archive = StrIEq( cmd, "seta" );
		RestOfLine( &p, end, out->value, sizeof( out->value ) );
		return out->kind = CFGOV_SET;
	}
	return out->kind = CFGOV_OTHER;
}

void CfgOv_ForEachLine( const char *text, cfgovLineFn fn, void *ctx )
{
	cfgovLine_t *l;

	if ( !text )
		return;
	l = malloc( sizeof( *l ) );
	if ( !l )
		return;
	while ( *text ) {
		const char *eol = text;
		while ( *eol && *eol != '\n' && *eol != '\r' )
			eol++;
		CfgOv_ParseLine( text, (size_t)( eol - text ), l );
		fn( l, text, (size_t)( eol - text ), ctx );
		text = eol;
		while ( *text == '\n' || *text == '\r' )
			text++;
	}
	free( l );
}

cfgovSetAction_t CfgOv_SetAction( int exists, int userCreated, int vmCreated )
{
	if ( exists && vmCreated )
		return CFGOV_KEEP_MOD;
	if ( exists && !userCreated )
		return CFGOV_APPLY_NOW;
	// only set by a config so far (the mod's, or none): whoever registers it decides
	return CFGOV_APPLY_ON_REGISTER;
}

/* ===================================================================== */

typedef struct {
	char	*buf;
	size_t	len, size;
	int		failed;
} strbuf_t;

static void Add( strbuf_t *b, const char *fmt, ... )
{
	va_list ap;
	int n;

	if ( b->failed )
		return;
	for ( ;; ) {
		va_start( ap, fmt );
		n = vsnprintf( b->buf ? b->buf + b->len : NULL, b->buf ? b->size - b->len : 0, fmt, ap );
		va_end( ap );
		if ( n < 0 ) {
			b->failed = 1;
			return;
		}
		if ( b->buf && b->len + (size_t)n < b->size ) {
			b->len += (size_t)n;
			return;
		}
		{
			size_t want = ( b->size ? b->size * 2 : 4096 ) + (size_t)n;
			char *nb = realloc( b->buf, want );
			if ( !nb ) {
				b->failed = 1;
				return;
			}
			b->buf = nb;
			b->size = want;
		}
	}
}

static void AddRaw( strbuf_t *b, const char *s, size_t n )
{
	Add( b, "%.*s\n", (int)n, s );
}

typedef struct {
	const cfgovBind_t	*changed;
	int					numChanged;
	const cfgovCvar_t	*engine;
	int					numEngine;
	char				*changedDone;	// per changed key: written
	char				*engineDone;	// per engine cvar: written
	strbuf_t			binds, sets, others;
} merge_t;

static void MergeLine( const cfgovLine_t *l, const char *raw, size_t rawLen, void *ctx )
{
	merge_t *m = ctx;
	int i;

	switch ( l->kind ) {
	case CFGOV_BIND:
		for ( i = 0; i < m->numChanged; i++ ) {
			if ( StrIEq( m->changed[i].key, l->name ) ) {
				if ( !m->changedDone[i] && m->changed[i].binding[0] )
					Add( &m->binds, "bind %s \"%s\"\n", l->name, m->changed[i].binding );
				m->changedDone[i] = 1;		// a later line for the same key is dropped
				return;
			}
		}
		Add( &m->binds, "bind %s \"%s\"\n", l->name, l->value );
		return;
	case CFGOV_SET:
		for ( i = 0; i < m->numEngine; i++ ) {
			if ( StrIEq( m->engine[i].name, l->name ) ) {
				Add( &m->sets, "%s %s \"%s\"\n", l->archive ? "seta" : "set", l->name, m->engine[i].value );
				m->engineDone[i] = 1;
				return;
			}
		}
		Add( &m->sets, "%s %s \"%s\"\n", l->archive ? "seta" : "set", l->name, l->value );
		return;
	case CFGOV_OTHER:
		AddRaw( &m->others, raw, rawLen );
		return;
	default:		// blank, comments, unbindall: the header has its own
		return;
	}
}

char *CfgOv_MergeBase( const char *baseText,
	const cfgovBind_t *changed, int numChanged,
	const cfgovCvar_t *engine, int numEngine )
{
	merge_t m;
	strbuf_t out;
	int i;

	memset( &m, 0, sizeof( m ) );
	memset( &out, 0, sizeof( out ) );
	m.changed = changed;
	m.numChanged = numChanged;
	m.engine = engine;
	m.numEngine = numEngine;
	m.changedDone = calloc( (size_t)numChanged + 1, 1 );
	m.engineDone = calloc( (size_t)numEngine + 1, 1 );
	if ( !m.changedDone || !m.engineDone ) {
		free( m.changedDone );
		free( m.engineDone );
		return NULL;
	}

	CfgOv_ForEachLine( baseText, MergeLine, &m );
	for ( i = 0; i < numChanged; i++ ) {
		if ( !m.changedDone[i] && changed[i].binding[0] )
			Add( &m.binds, "bind %s \"%s\"\n", changed[i].key, changed[i].binding );
	}
	for ( i = 0; i < numEngine; i++ ) {
		if ( !m.engineDone[i] )
			Add( &m.sets, "seta %s \"%s\"\n", engine[i].name, engine[i].value );
	}

	Add( &out, "// generated by quake, do not modify\nunbindall\n" );
	Add( &out, "%s", m.binds.buf ? m.binds.buf : "" );
	Add( &out, "%s", m.sets.buf ? m.sets.buf : "" );
	Add( &out, "%s", m.others.buf ? m.others.buf : "" );

	if ( m.binds.failed || m.sets.failed || m.others.failed ) {
		out.failed = 1;
	}
	free( m.binds.buf );
	free( m.sets.buf );
	free( m.others.buf );
	free( m.changedDone );
	free( m.engineDone );
	if ( out.failed ) {
		free( out.buf );
		return NULL;
	}
	return out.buf;
}
