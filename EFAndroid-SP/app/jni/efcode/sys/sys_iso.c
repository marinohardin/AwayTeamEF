/*
===========================================================================
The game files from an image of the Elite Force CD.

This file is part of VoyagerSP (GPLv2 or later, like the rest of the engine).

The launchers' Choose… and Add Game Files… take a CD image as well as a
folder or GOG's installer: the game's own CD (BaseEF/pak0.pk3 under SETUP)
or the Expansion Pack's (pak1-pak3). Both store the .pk3 files as plain
files of an ISO 9660 file system, so they are copied straight out of the
image; nothing is installed or run, and the copy-protection files on the
game's CD are never touched.

Accepted:
  - .iso / .img / .cdr: 2048-byte sectors (what most tools make);
  - .bin: raw 2352-byte sectors (MODE1/2352, or MODE2/2352 form 1);
  - .cue: the .bin its first FILE line names.
The layout is found from the content (the "CD001" volume descriptor), not
from the name. Only the primary volume descriptor is read (the 8.3 names of
the .pk3 files are all that's needed; they are lower-cased like a normal
install). Every length and offset read from the image is checked against
the image before it is used. A .pk3 has to end in a zip end-of-central-
directory record, so a misread image is refused before anything is copied.

Portable C with no dependencies: the Mac launcher (sys_osx_data.m) and the
Windows/Linux launcher core (efcode/launcher, lc_data.c) both call it.
Paths are UTF-8 everywhere (wide-character file APIs on Windows).
===========================================================================
*/

#include "sys_iso.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#ifdef _WIN32
#	include <windows.h>
#	include <sys/utime.h>
#	define ISO_SEEK		_fseeki64
#	define ISO_TELL		_ftelli64
#else
#	include <strings.h>
#	include <utime.h>
#	define ISO_SEEK		fseeko
#	define ISO_TELL		ftello
#endif

#define ISO_BLOCK		2048
#define MAX_DIR_BYTES	( 4u << 20 )	// one directory's records
#define MAX_DIRS		4096
#define MAX_DEPTH		6
#define MAX_FILES		256
#define RAW_CHUNK		256				// sectors per read of a raw image
#define ZIP_TAIL		( 65535 + 22 )	// a zip comment's maximum + the end record

static void Fail( char *err, size_t errSize, const char *fmt, ... )
{
	va_list ap;

	if( !err || !errSize )
		return;
	va_start( ap, fmt );
	vsnprintf( err, errSize, fmt, ap );
	va_end( ap );
}

static int ICmp( const char *a, const char *b )
{
	for( ; *a && *b; a++, b++ )
		if( tolower( (unsigned char)*a ) != tolower( (unsigned char)*b ) )
			break;
	return tolower( (unsigned char)*a ) - tolower( (unsigned char)*b );
}

static int EndsWith( const char *s, const char *suffix )
{
	size_t n = strlen( s ), m = strlen( suffix );

	return n >= m && !ICmp( s + n - m, suffix );
}

/* ---- files on UTF-8 paths ------------------------------------------------ */

#ifdef _WIN32
static int Wide( const char *path, wchar_t *w, int count )
{
	return MultiByteToWideChar( CP_UTF8, 0, path, -1, w, count ) > 0;
}
#endif

static FILE *Open( const char *path, const char *mode )
{
#ifdef _WIN32
	wchar_t w[1024], m[8];

	if( !Wide( path, w, 1024 ) || !Wide( mode, m, 8 ) )
	{
		errno = ENOENT;
		return NULL;
	}
	return _wfopen( w, m );
#else
	return fopen( path, mode );
#endif
}

static void Remove( const char *path )
{
#ifdef _WIN32
	wchar_t w[1024];

	if( Wide( path, w, 1024 ) )
		_wremove( w );
#else
	remove( path );
#endif
}

static int RenameOver( const char *from, const char *to )
{
#ifdef _WIN32
	wchar_t wf[1024], wt[1024];

	if( !Wide( from, wf, 1024 ) || !Wide( to, wt, 1024 ) )
		return 0;
	if( MoveFileExW( wf, wt, MOVEFILE_REPLACE_EXISTING ) )
		return 1;
	errno = EACCES;
	return 0;
#else
	return rename( from, to ) == 0;
#endif
}

static void SetTime( const char *path, int64_t mtime )
{
	if( mtime < 0 )
		return;
#ifdef _WIN32
	{
		wchar_t w[1024];
		struct _utimbuf t;

		t.actime = t.modtime = (time_t)mtime;
		if( Wide( path, w, 1024 ) )
			_wutime( w, &t );
	}
#else
	{
		struct utimbuf t;

		t.actime = t.modtime = (time_t)mtime;
		utime( path, &t );
	}
#endif
}

static int ReadAt( FILE *f, uint64_t at, void *buf, size_t n )
{
	if( ISO_SEEK( f, (int64_t)at, SEEK_SET ) )
		return 0;
	return fread( buf, 1, n, f ) == n;
}

static uint64_t FileSize( FILE *f )
{
	int64_t n;

	if( ISO_SEEK( f, 0, SEEK_END ) || ( n = ISO_TELL( f ) ) < 0 )
		return 0;
	return (uint64_t)n;
}

/* ---- the image's data, 2048 bytes a sector ------------------------------ */

typedef struct {
	FILE			*f;
	uint64_t		size;		// of the image file
	uint32_t		ss, off;	// sector size, data offset in a sector
	unsigned char	*raw;		// RAW_CHUNK raw sectors (raw images only)
} reader_t;

// n bytes of the data area starting at byte at (sector * 2048 + offset)
static int ReadData( reader_t *r, uint64_t at, void *buf, size_t n )
{
	unsigned char *out = buf;

	if( r->ss == ISO_BLOCK )
		return at + n <= r->size && ReadAt( r->f, at, buf, n );
	while( n )
	{
		uint64_t sector = at / ISO_BLOCK;
		size_t inner = (size_t)( at % ISO_BLOCK ), count, i;

		count = ( inner + n + ISO_BLOCK - 1 ) / ISO_BLOCK;
		if( count > RAW_CHUNK )
			count = RAW_CHUNK;
		if( ( sector + count ) * r->ss > r->size || !ReadAt( r->f, sector * r->ss, r->raw, count * r->ss ) )
			return 0;
		for( i = 0; i < count && n; i++ )
		{
			size_t take = ISO_BLOCK - inner;

			if( take > n )
				take = n;
			memcpy( out, r->raw + i * r->ss + r->off + inner, take );
			out += take;
			at += take;
			n -= take;
			inner = 0;
		}
	}
	return 1;
}

static int ReaderOpen( reader_t *r, const char *path, uint32_t ss, uint32_t off )
{
	memset( r, 0, sizeof( *r ) );
	if( !( r->f = Open( path, "rb" ) ) )
		return 0;
	r->size = FileSize( r->f );
	r->ss = ss;
	r->off = off;
	if( ss != ISO_BLOCK && !( r->raw = malloc( (size_t)RAW_CHUNK * ss ) ) )
	{
		fclose( r->f );
		r->f = NULL;
		errno = ENOMEM;
		return 0;
	}
	return 1;
}

static void ReaderClose( reader_t *r )
{
	if( r->f )
		fclose( r->f );
	free( r->raw );
	memset( r, 0, sizeof( *r ) );
}

/* ---- names ---------------------------------------------------------------- */

int ISO_IsImageName( const char *path )
{
	static const char *exts[] = { ".iso", ".bin", ".img", ".cue", ".cdr", NULL };
	int i;

	if( !path )
		return 0;
	for( i = 0; exts[i]; i++ )
		if( EndsWith( path, exts[i] ) )
			return 1;
	return 0;
}

// a .cue sheet's first FILE, next to the sheet; 0 if it names none
static int CueTarget( const char *cue, char *out, size_t outSize )
{
	char text[16384], name[1024];
	FILE *f = Open( cue, "rb" );
	size_t n, i, k;
	const char *p;

	if( !f )
		return 0;
	n = fread( text, 1, sizeof( text ) - 1, f );
	fclose( f );
	text[n] = 0;
	for( p = text; *p; )
	{
		while( *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' )
			p++;
		if( strncmp( p, "FILE", 4 ) || ( p[4] != ' ' && p[4] != '\t' ) )
		{
			while( *p && *p != '\n' )
				p++;
			continue;
		}
		p += 4;
		while( *p == ' ' || *p == '\t' )
			p++;
		k = 0;
		if( *p == '"' )
			for( p++; *p && *p != '"' && *p != '\n' && k < sizeof( name ) - 1; )
				name[k++] = *p++;
		else
			while( *p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && k < sizeof( name ) - 1 )
				name[k++] = *p++;
		name[k] = 0;
		if( !k )
			return 0;
		// the name's own folder part is dropped: it's next to the sheet
		for( i = k; i > 0 && name[i - 1] != '/' && name[i - 1] != '\\'; i-- )
			;
		for( k = strlen( cue ); k > 0 && cue[k - 1] != '/' && cue[k - 1] != '\\'; k-- )
			;
		if( k + strlen( name + i ) + 1 > outSize )
			return 0;
		memcpy( out, cue, k );
		strcpy( out + k, name + i );
		return 1;
	}
	return 0;
}

/* ---- the file system ---------------------------------------------------- */

static uint32_t LE32( const unsigned char *p )
{
	return p[0] | ( p[1] << 8 ) | ( p[2] << 16 ) | ( (uint32_t)p[3] << 24 );
}

// days since 1970-01-01 of a proleptic Gregorian date
static int64_t DaysFromCivil( int y, int m, int d )
{
	int64_t era, yoe, doy, doe;

	y -= m <= 2;
	era = ( y >= 0 ? y : y - 399 ) / 400;
	yoe = y - era * 400;
	doy = ( 153 * ( m + ( m > 2 ? -3 : 9 ) ) + 2 ) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

// a directory record's 7-byte date (years since 1900, month, day, h, m, s, GMT offset in 15 min)
static int64_t RecordTime( const unsigned char *t )
{
	if( t[1] < 1 || t[1] > 12 || t[2] < 1 || t[2] > 31 || t[3] > 23 || t[4] > 59 || t[5] > 60 )
		return -1;
	return DaysFromCivil( 1900 + t[0], t[1], t[2] ) * 86400 + t[3] * 3600 + t[4] * 60 + t[5] -
		(int64_t)(signed char)t[6] * 15 * 60;
}

typedef struct {
	reader_t	*r;
	uint64_t	blocks;		// data sectors in the image
	isoImage_t	*img;
	int			dirs;
	int			damaged;
} walk_t;

// the record's name without ";1" and a trailing '.', in name (64 bytes); 0 if unusable
static int RecordName( const unsigned char *rec, char *name )
{
	int n = rec[32], i;

	if( n <= 0 || n >= 64 )
		return 0;
	memcpy( name, rec + 33, (size_t)n );
	name[n] = 0;
	for( i = 0; i < n; i++ )
		if( name[i] == ';' )
		{
			name[i] = 0;
			break;
		}
	n = (int)strlen( name );
	if( n && name[n - 1] == '.' )
		name[--n] = 0;
	if( !n || name[0] == '.' )
		return 0;
	for( i = 0; i < n; i++ )
		if( name[i] == '/' || name[i] == '\\' || name[i] == ':' || (unsigned char)name[i] < 0x20 )
			return 0;
	return 1;
}

static void AddFile( walk_t *w, const char *name, uint32_t sector, uint32_t size, int64_t mtime )
{
	isoImage_t *img = w->img;
	isoFile_t *grown, *f;
	int i;

	for( i = 0; i < img->numFiles; i++ )
		if( !ICmp( img->files[i].name, name ) )
			return;		// the first BaseEF found wins
	if( img->numFiles >= MAX_FILES )
		return;
	if( !( grown = realloc( img->files, sizeof( *grown ) * (size_t)( img->numFiles + 1 ) ) ) )
		return;
	img->files = grown;
	f = &img->files[img->numFiles++];
	memset( f, 0, sizeof( *f ) );
	for( i = 0; name[i] && i < (int)sizeof( f->name ) - 1; i++ )
		f->name[i] = (char)tolower( (unsigned char)name[i] );
	f->sector = sector;
	f->size = size;
	f->mtime = mtime;
	if( !strcmp( f->name, "pak0.pk3" ) )
		img->hasPak0 = 1;
}

static void Walk( walk_t *w, uint32_t sector, uint32_t length, int depth, int inBase )
{
	unsigned char *data;
	uint32_t o = 0;

	if( depth > MAX_DEPTH || ++w->dirs > MAX_DIRS )
		return;
	if( !length || length > MAX_DIR_BYTES || sector + (uint64_t)( length + ISO_BLOCK - 1 ) / ISO_BLOCK > w->blocks )
	{
		w->damaged = 1;
		return;
	}
	if( !( data = malloc( length ) ) )
		return;
	if( !ReadData( w->r, (uint64_t)sector * ISO_BLOCK, data, length ) )
	{
		w->damaged = 1;
		free( data );
		return;
	}
	while( o < length )
	{
		const unsigned char *rec = data + o;
		uint32_t len = rec[0], child, size;
		char name[64];

		if( !len )
		{
			o = ( o / ISO_BLOCK + 1 ) * ISO_BLOCK;		// records don't cross sectors
			continue;
		}
		if( len < 34 || o + len > length || 33u + rec[32] > len )
		{
			w->damaged = 1;
			break;
		}
		o += len;
		if( rec[32] == 1 && ( rec[33] == 0 || rec[33] == 1 ) )
			continue;		// . and ..
		if( !RecordName( rec, name ) )
			continue;
		child = LE32( rec + 2 );
		size = LE32( rec + 10 );
		if( rec[25] & 0x02 )
		{
			if( child != sector )
				Walk( w, child, size, depth + 1, !ICmp( name, "BaseEF" ) );
			continue;
		}
		if( !inBase || ( rec[25] & 0x80 ) || !EndsWith( name, ".pk3" ) )	// 0x80: split into extents (never a .pk3 on the CDs)
			continue;
		if( child + (uint64_t)( size + ISO_BLOCK - 1 ) / ISO_BLOCK > w->blocks )
		{
			w->damaged = 1;
			continue;
		}
		AddFile( w, name, child, size, RecordTime( rec + 18 ) );
	}
	free( data );
}

// 1 when the data area holds a zip end record within its last 64 KB
static int EndsLikeZip( reader_t *r, const isoFile_t *f )
{
	size_t n = f->size < ZIP_TAIL ? (size_t)f->size : ZIP_TAIL, i;
	unsigned char *tail;
	int ok = 0;

	if( n < 22 || !( tail = malloc( n ) ) )
		return 0;
	if( ReadData( r, f->sector * ISO_BLOCK + f->size - n, tail, n ) )
		for( i = n - 22 + 1; i-- > 0 && !ok; )
			ok = tail[i] == 'P' && tail[i + 1] == 'K' && tail[i + 2] == 5 && tail[i + 3] == 6;
	free( tail );
	return ok;
}

void ISO_Free( isoImage_t *img )
{
	if( !img )
		return;
	free( img->files );
	img->files = NULL;
	img->numFiles = 0;
}

int ISO_ReadImage( const char *path, isoImage_t *img, char *err, size_t errSize )
{
	static const uint32_t layouts[][2] = { { 2048, 0 }, { 2352, 16 }, { 2352, 24 } };
	unsigned char vd[ISO_BLOCK];
	reader_t r;
	walk_t w;
	int layout, i, found = 0;

	memset( img, 0, sizeof( *img ) );
	if( err && errSize )
		err[0] = 0;
	if( EndsWith( path, ".cue" ) )
	{
		if( !CueTarget( path, img->image, sizeof( img->image ) ) )
			return Fail( err, errSize, "its .cue sheet names no image file" ), 0;
	}
	else if( strlen( path ) < sizeof( img->image ) )
		strcpy( img->image, path );
	else
		return Fail( err, errSize, "its path is too long" ), 0;

	// the layout: a primary volume descriptor at sector 16
	for( layout = 0; layout < 3 && !found; layout++ )
	{
		if( !ReaderOpen( &r, img->image, layouts[layout][0], layouts[layout][1] ) )
			return Fail( err, errSize, "it can't be read (%s)", strerror( errno ) ), 0;
		for( i = 16; i < 32; i++ )
		{
			if( !ReadData( &r, (uint64_t)i * ISO_BLOCK, vd, ISO_BLOCK ) || memcmp( vd + 1, "CD001", 5 ) || vd[0] == 255 )
				break;
			if( vd[0] == 1 )
			{
				found = 1;
				break;
			}
		}
		if( !found )
			ReaderClose( &r );
	}
	if( !found )
		return Fail( err, errSize, "it isn't a CD image this app can read (no ISO 9660 file system)" ), 0;
	img->sectorSize = r.ss;
	img->dataOffset = r.off;
	if( ( vd[128] | ( vd[129] << 8 ) ) != ISO_BLOCK )
	{
		ReaderClose( &r );
		return Fail( err, errSize, "its file system uses a block size this app doesn't know" ), 0;
	}
	memcpy( img->label, vd + 40, 32 );
	for( i = 31; i >= 0 && ( img->label[i] == ' ' || img->label[i] == 0 ); i-- )
		img->label[i] = 0;

	memset( &w, 0, sizeof( w ) );
	w.r = &r;
	w.img = img;
	w.blocks = r.size / r.ss;
	if( vd[156] < 34 )
	{
		ReaderClose( &r );
		return Fail( err, errSize, "its file system is damaged" ), 0;
	}
	Walk( &w, LE32( vd + 156 + 2 ), LE32( vd + 156 + 10 ), 0, 0 );
	if( !img->numFiles )
	{
		ReaderClose( &r );
		ISO_Free( img );
		return Fail( err, errSize, w.damaged ? "the image is incomplete or damaged" :
			"it has no BaseEF folder with .pk3 files (it isn't Elite Force's CD or the Expansion Pack's)" ), 0;
	}
	for( i = 0; i < img->numFiles; i++ )
		if( !EndsLikeZip( &r, &img->files[i] ) )
		{
			Fail( err, errSize, "%s on it can't be read (the image is incomplete or damaged)", img->files[i].name );
			ReaderClose( &r );
			ISO_Free( img );
			return 0;
		}
	ReaderClose( &r );
	return 1;
}

#define COPY_BYTES	( 1u << 20 )

int ISO_ExtractFile( const isoImage_t *img, const isoFile_t *f, const char *to,
	volatile unsigned long long *progress, volatile signed char *cancel, char *err, size_t errSize )
{
	char part[1100];
	unsigned char *buf = malloc( COPY_BYTES );
	reader_t r;
	FILE *out = NULL;
	uint64_t done = 0;
	int ok = 0, open = 0;

	snprintf( part, sizeof( part ), "%s.part", to );
	if( !buf )
	{
		Fail( err, errSize, "Out of memory" );
		return 0;
	}
	if( !( open = ReaderOpen( &r, img->image, img->sectorSize, img->dataOffset ) ) )
	{
		Fail( err, errSize, "Can't read the disc image: %s", strerror( errno ) );
		goto done;
	}
	if( !( out = Open( part, "wb" ) ) )
	{
		Fail( err, errSize, "Can't write %s: %s", part, strerror( errno ) );
		goto done;
	}
	while( done < f->size )
	{
		size_t n = f->size - done < COPY_BYTES ? (size_t)( f->size - done ) : COPY_BYTES;

		if( cancel && *cancel )
		{
			Fail( err, errSize, "Cancelled." );
			goto done;
		}
		if( !ReadData( &r, f->sector * ISO_BLOCK + done, buf, n ) )
		{
			Fail( err, errSize, "Can't read %s from the disc image (it's incomplete or damaged)", f->name );
			goto done;
		}
		if( fwrite( buf, 1, n, out ) != n )
		{
			Fail( err, errSize, "Can't write %s: %s", part, strerror( errno ) );
			goto done;
		}
		done += n;
		if( progress )
			*progress += n;
	}
	if( fclose( out ) )
	{
		out = NULL;
		Fail( err, errSize, "Can't write %s: %s", part, strerror( errno ) );
		goto done;
	}
	out = NULL;
	SetTime( part, f->mtime );
	if( !RenameOver( part, to ) )
	{
		Fail( err, errSize, "Can't rename %s: %s", part, strerror( errno ) );
		goto done;
	}
	ok = 1;
done:
	if( out )
		fclose( out );
	if( !ok )
		Remove( part );
	if( open )
		ReaderClose( &r );
	free( buf );
	return ok;
}
