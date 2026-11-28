/*
===========================================================================
liblauncher.so: the Quest start screen's bridge to the desktop launchers' core
(EFAndroid-SP/app/jni/efcode/launcher, docs/launcher-spec). Java side:
android/java-quest/org/awayteamef/app/LauncherCore.java.

The start screen runs in the app's main process; the game (libmain.so) runs in ":game".
This small library carries only the core: the game files' checks, the copy and unpack
jobs of Choose / Add Game Files (CD images through sys_iso.c, GOG's installer through
sys_osx_gog.c when liblzma was built), the server browser model (master query, getinfo /
getstatus, sorting, filters, the status texts) and the co-op character table.

Handles are pointers passed to Java as jlong. Blocking calls (a copy job) are made from a
Java worker thread; the browser's fetches run on the core's own worker threads and bump a
change counter that the UI polls (no JNI calls from the core's threads).

This file is part of AwayTeamEF (GPLv2 or later, like the rest of the engine).
===========================================================================
*/

#define _BSD_SOURCE 1	/* funopen64 */

#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <android/log.h>

#include "lc_common.h"
#include "lc_data.h"
#include "lc_server.h"
#include "lc_net.h"
#include "lc_nat.h"
#include "lc_coop.h"
#include "lc_settings.h"

#define TAG "AwayTeamEF"
#define JNI_FN( name ) Java_org_awayteamef_app_LauncherCore_##name

/*
===========================================================================
Picked files. A file picked with Android's picker is a content:// URI; Java opens it
(ContentResolver.openFileDescriptor) and registers the descriptor here under a path in
the app's cache folder named like the picked file. The core reads plain paths, and
reopening /proc/self/fd/<n> of a shared-storage file is refused (EACCES), so the core's
fopen / open / stat calls are wrapped at link time (-Wl,--wrap, liblauncher only; the
core's sources are unchanged): a registered path is served from its descriptor, read only,
each fopen with its own position (pread), anything else goes to the real call.
===========================================================================
*/

#define MAX_PICKED 64

typedef struct {
	char	path[1024];
	int		fd;
} picked_t;

static picked_t picked[MAX_PICKED];
static int numPicked;
static pthread_mutex_t pickedLock = PTHREAD_MUTEX_INITIALIZER;

/* a dup of the registered descriptor for path, or -1 */
static int PickedDup( const char *path )
{
	int fd = -1;

	if ( !path )
		return -1;
	pthread_mutex_lock( &pickedLock );
	for ( int i = 0; i < numPicked; i++ ) {
		if ( !strcmp( picked[i].path, path ) ) {
			fd = fcntl( picked[i].fd, F_DUPFD_CLOEXEC, 0 );
			break;
		}
	}
	pthread_mutex_unlock( &pickedLock );
	return fd;
}

typedef struct {
	int		fd;
	off64_t	pos;
} pickedFile_t;

static int PickedRead( void *c, char *buf, int n )
{
	pickedFile_t *f = c;
	ssize_t r = pread64( f->fd, buf, (size_t)n, f->pos );

	if ( r > 0 )
		f->pos += r;
	return (int)r;
}

static off64_t PickedSeek( void *c, off64_t off, int whence )
{
	pickedFile_t *f = c;
	struct stat st;
	off64_t base = 0;

	if ( whence == SEEK_CUR )
		base = f->pos;
	else if ( whence == SEEK_END ) {
		if ( fstat( f->fd, &st ) )
			return -1;
		base = st.st_size;
	}
	if ( base + off < 0 ) {
		errno = EINVAL;
		return -1;
	}
	return f->pos = base + off;
}

static int PickedClose( void *c )
{
	pickedFile_t *f = c;

	close( f->fd );
	free( f );
	return 0;
}

FILE *__real_fopen( const char *path, const char *mode );
int __real_open( const char *path, int flags, ... );
int __real_stat( const char *path, struct stat *st );

FILE *__wrap_fopen( const char *path, const char *mode )
{
	if ( mode && mode[0] == 'r' && !strchr( mode, '+' ) ) {
		int fd = PickedDup( path );

		if ( fd >= 0 ) {
			pickedFile_t *f = calloc( 1, sizeof( *f ) );
			FILE *fp = f ? funopen64( f, PickedRead, NULL, PickedSeek, PickedClose ) : NULL;

			if ( !fp ) {
				free( f );
				close( fd );
				errno = ENOMEM;
				return NULL;
			}
			f->fd = fd;
			return fp;
		}
	}
	return __real_fopen( path, mode );
}

int __wrap_open( const char *path, int flags, ... )
{
	mode_t mode = 0;

	if ( ( flags & O_ACCMODE ) == O_RDONLY && !( flags & O_CREAT ) ) {
		int fd = PickedDup( path );

		if ( fd >= 0 ) {	/* sys_osx_gog.c reads with pread; the shared position isn't used */
			lseek( fd, 0, SEEK_SET );
			return fd;
		}
	}
	if ( flags & O_CREAT ) {
		va_list ap;

		va_start( ap, flags );
		mode = (mode_t)va_arg( ap, int );
		va_end( ap );
	}
	return __real_open( path, flags, mode );
}

int __wrap_stat( const char *path, struct stat *st )
{
	int fd = PickedDup( path );

	if ( fd >= 0 ) {
		int r = fstat( fd, st );

		close( fd );
		return r;
	}
	return __real_stat( path, st );
}

JNIEXPORT void JNICALL JNI_FN( pickedAdd )( JNIEnv *env, jclass c, jstring jpath, jint fd )
{
	const char *path = (*env)->GetStringUTFChars( env, jpath, NULL );

	(void)c;
	pthread_mutex_lock( &pickedLock );
	if ( path && numPicked < MAX_PICKED && strlen( path ) < sizeof( picked[0].path ) ) {
		strcpy( picked[numPicked].path, path );
		picked[numPicked].fd = fd;
		numPicked++;
	}
	pthread_mutex_unlock( &pickedLock );
	if ( path )
		(*env)->ReleaseStringUTFChars( env, jpath, path );
}

/* the descriptors themselves belong to Java (ParcelFileDescriptor) and are closed there */
JNIEXPORT void JNICALL JNI_FN( pickedClear )( JNIEnv *env, jclass c )
{
	(void)env; (void)c;
	pthread_mutex_lock( &pickedLock );
	numPicked = 0;
	pthread_mutex_unlock( &pickedLock );
}

static void LogSink( const char *line, void *user )
{
	(void)user;
	__android_log_print( ANDROID_LOG_INFO, TAG, "launcher: %s", line );
}

static jobjectArray StringArray( JNIEnv *env, int n )
{
	jclass sc = (*env)->FindClass( env, "java/lang/String" );
	return (*env)->NewObjectArray( env, n, sc, NULL );
}

static void SetString( JNIEnv *env, jobjectArray a, int i, const char *s )
{
	jstring j = (*env)->NewStringUTF( env, s ? s : "" );
	(*env)->SetObjectArrayElement( env, a, i, j );
	(*env)->DeleteLocalRef( env, j );
}

/* out[0] = title, out[1] = text (an alert); out may be NULL or shorter */
static void SetAlert( JNIEnv *env, jobjectArray out, const char *title, const char *text )
{
	if ( !out || (*env)->GetArrayLength( env, out ) < 2 )
		return;
	SetString( env, out, 0, title );
	SetString( env, out, 1, text );
}

/* ---- setup ---------------------------------------------------------------------- */

/* obbDir: the app's OBB folder, the engine's homepath (baseEF inside it) */
JNIEXPORT void JNICALL JNI_FN( nativeInit )( JNIEnv *env, jclass cls, jstring obbDir )
{
	const char *o = (*env)->GetStringUTFChars( env, obbDir, NULL );
	(void)cls;
	setenv( "LC_HOMEPATH", o, 1 );
	(*env)->ReleaseStringUTFChars( env, obbDir, o );
	lc_set_log_sink( LogSink, NULL );
}

/* ---- game files ----------------------------------------------------------------- */

JNIEXPORT jboolean JNICALL JNI_FN( hasExpansion )( JNIEnv *env, jclass cls, jstring homeBase )
{
	const char *h = (*env)->GetStringUTFChars( env, homeBase, NULL );
	int r = lc_data_has_expansion( h, h );
	(void)cls;
	(*env)->ReleaseStringUTFChars( env, homeBase, h );
	return r ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL JNI_FN( haveGog )( JNIEnv *env, jclass cls )
{
	(void)env; (void)cls;
	return lc_have_gog( ) ? JNI_TRUE : JNI_FALSE;
}

typedef struct {
	lc_copyjob *job;
	volatile unsigned long long progress;
	volatile signed char cancel;
	int unpackInstaller;		/* for the "Nothing to add" text: 0 files, 1 GOG, 2 disc */
} job_t;

static jlong WrapJob( lc_copyjob *j, int kind )
{
	job_t *w;

	if ( !j )
		return 0;
	w = calloc( 1, sizeof( *w ) );
	if ( !w ) {
		lc_copyjob_free( j );
		return 0;
	}
	w->job = j;
	w->unpackInstaller = kind;
	return (jlong)(intptr_t)w;
}

/* A CD image or GOG's installer (path: a name ending in .iso/.bin/.cue/.img/.cdr/.exe).
   0 = nothing to run: out = the alert (can't be used / nothing to add). */
JNIEXPORT jlong JNICALL JNI_FN( planUnpack )( JNIEnv *env, jclass cls, jstring path, jboolean onlyNew,
	jstring homeBase, jobjectArray out )
{
	const char *p = (*env)->GetStringUTFChars( env, path, NULL );
	const char *h = (*env)->GetStringUTFChars( env, homeBase, NULL );
	char err[512] = "", title[256], text[2048];
	int present = 0, disc = lc_is_disc_image( p );
	lc_copyjob *j = lc_plan_unpack_installer( p, onlyNew ? 1 : 0, h, "", &present, err, sizeof( err ) );
	jlong r;

	(void)cls;
	if ( !j ) {
		if ( err[0] )
			lc_installer_error_alert( p, err, title, sizeof( title ), text, sizeof( text ) );
		else
			lc_nothing_to_add_alert( disc ? 2 : 1, present, title, sizeof( title ), text, sizeof( text ) );
		SetAlert( env, out, title, text );
	}
	r = WrapJob( j, disc ? 2 : 1 );
	(*env)->ReleaseStringUTFChars( env, path, p );
	(*env)->ReleaseStringUTFChars( env, homeBase, h );
	return r;
}

/* .pk3 files (each a path; a folder's .pk3s are passed one by one) */
JNIEXPORT jlong JNICALL JNI_FN( planAdd )( JNIEnv *env, jclass cls, jobjectArray paths, jstring homeBase,
	jobjectArray out )
{
	int n = (*env)->GetArrayLength( env, paths ), i, present = 0;
	const char **picked = calloc( n ? n : 1, sizeof( *picked ) );
	jstring *js = calloc( n ? n : 1, sizeof( *js ) );
	const char *h = (*env)->GetStringUTFChars( env, homeBase, NULL );
	char title[256], text[2048];
	lc_copyjob *j;

	(void)cls;
	for ( i = 0; i < n; i++ ) {
		js[i] = (jstring)(*env)->GetObjectArrayElement( env, paths, i );
		picked[i] = (*env)->GetStringUTFChars( env, js[i], NULL );
	}
	j = lc_plan_add_files( picked, n, h, "", &present );
	if ( j && lc_copyjob_count( j ) == 0 ) {	/* every picked .pk3 is there already */
		lc_copyjob_free( j );
		j = NULL;
	}
	if ( !j ) {
		lc_nothing_to_add_alert( 0, present, title, sizeof( title ), text, sizeof( text ) );
		SetAlert( env, out, title, text );
	}
	for ( i = 0; i < n; i++ ) {
		(*env)->ReleaseStringUTFChars( env, js[i], picked[i] );
		(*env)->DeleteLocalRef( env, js[i] );
	}
	(*env)->ReleaseStringUTFChars( env, homeBase, h );
	free( picked );
	free( js );
	return WrapJob( j, 0 );
}

JNIEXPORT jboolean JNICALL JNI_FN( jobCheckSpace )( JNIEnv *env, jclass cls, jlong h, jobjectArray out )
{
	job_t *w = (job_t *)(intptr_t)h;
	char title[256], text[2048];

	(void)cls;
	if ( lc_copyjob_check_space( w->job, title, sizeof( title ), text, sizeof( text ) ) )
		return JNI_TRUE;
	SetAlert( env, out, title, text );
	return JNI_FALSE;
}

/* [0] the progress panel's title, [1] its label, [2] the number of files */
JNIEXPORT jobjectArray JNICALL JNI_FN( jobTexts )( JNIEnv *env, jclass cls, jlong h )
{
	job_t *w = (job_t *)(intptr_t)h;
	char title[256], label[2048], count[32];
	jobjectArray a = StringArray( env, 3 );

	(void)cls;
	lc_copyjob_title( w->job, title, sizeof( title ) );
	lc_copyjob_progress_text( w->job, 0, label, sizeof( label ) );
	lc_snprintf( count, sizeof( count ), "%d", lc_copyjob_count( w->job ) );
	SetString( env, a, 0, title );
	SetString( env, a, 1, label );
	SetString( env, a, 2, count );
	return a;
}

JNIEXPORT jlong JNICALL JNI_FN( jobTotal )( JNIEnv *env, jclass cls, jlong h )
{
	(void)env; (void)cls;
	return (jlong)lc_copyjob_total( ((job_t *)(intptr_t)h)->job );
}

JNIEXPORT jlong JNICALL JNI_FN( jobProgress )( JNIEnv *env, jclass cls, jlong h )
{
	(void)env; (void)cls;
	return (jlong)((job_t *)(intptr_t)h)->progress;
}

JNIEXPORT void JNICALL JNI_FN( jobCancel )( JNIEnv *env, jclass cls, jlong h )
{
	(void)env; (void)cls;
	((job_t *)(intptr_t)h)->cancel = 1;
}

/* blocking (a worker thread): 1 done, 0 failed (out = the failure alert), -1 cancelled */
JNIEXPORT jint JNICALL JNI_FN( jobRun )( JNIEnv *env, jclass cls, jlong h, jobjectArray out )
{
	job_t *w = (job_t *)(intptr_t)h;
	char err[1024] = "", title[256], text[2048];
	int r;

	(void)cls;
	r = lc_copyjob_run( w->job, &w->progress, &w->cancel, err, sizeof( err ) );
	if ( r == 0 ) {
		lc_copyjob_failure_texts( w->job, err, title, sizeof( title ), text, sizeof( text ) );
		SetAlert( env, out, title, text );
	}
	return r;
}

JNIEXPORT void JNICALL JNI_FN( jobFree )( JNIEnv *env, jclass cls, jlong h )
{
	job_t *w = (job_t *)(intptr_t)h;

	(void)env; (void)cls;
	if ( !w )
		return;
	lc_copyjob_free( w->job );
	free( w );
}

/* ---- the server browser (servers.md §8) ------------------------------------------- */

typedef struct {
	lc_browser *b;
	volatile int changes;		/* bumped by the fetch workers; the UI polls it */
	lc_sort_key keys[LC_COL_PING + 1];
	int nKeys;
	const lc_map_catalog *maps;
} browser_t;

static void BrowserChanged( int source, void *u )
{
	(void)source;
	__atomic_add_fetch( &((browser_t *)u)->changes, 1, __ATOMIC_SEQ_CST );
}

JNIEXPORT jlong JNICALL JNI_FN( browserNew )( JNIEnv *env, jclass cls, jstring homeBase )
{
	const char *h = (*env)->GetStringUTFChars( env, homeBase, NULL );
	char masters[LC_MAX_MASTERS][65];
	int n;
	browser_t *w = calloc( 1, sizeof( *w ) );

	(void)cls;
	if ( !w ) {
		(*env)->ReleaseStringUTFChars( env, homeBase, h );
		return 0;
	}
	/* the desktop's default master list (no Servers… sheet on the Quest) */
	n = lc_clean_masters( (const char *const *)lc_default_masters, 5, masters );
	w->maps = lc_map_catalog_get( h, h );
	w->b = lc_browser_new( (const char (*)[65])masters, n, w->maps );
	lc_browser_default_sort( w->keys );
	w->nKeys = 4;
	/* the Loopback Host Fix's public address (an internet fetch waits up to 1 s for it) */
	lc_public_address_start( );
	(*env)->ReleaseStringUTFChars( env, homeBase, h );
	return (jlong)(intptr_t)w;
}

JNIEXPORT void JNICALL JNI_FN( browserFree )( JNIEnv *env, jclass cls, jlong h )
{
	browser_t *w = (browser_t *)(intptr_t)h;

	(void)env; (void)cls;
	if ( !w )
		return;
	lc_browser_free( w->b );
	free( w );
}

JNIEXPORT jobjectArray JNICALL JNI_FN( browserSources )( JNIEnv *env, jclass cls, jlong h )
{
	browser_t *w = (browser_t *)(intptr_t)h;
	int n = lc_browser_source_count( w->b ), i;
	jobjectArray a = StringArray( env, n );
	char label[64];

	(void)cls;
	for ( i = 0; i < n; i++ ) {
		lc_browser_source_label( w->b, i, label, sizeof( label ) );
		SetString( env, a, i, label );
	}
	return a;
}

JNIEXPORT void JNICALL JNI_FN( browserFetch )( JNIEnv *env, jclass cls, jlong h, jint source )
{
	browser_t *w = (browser_t *)(intptr_t)h;

	(void)env; (void)cls;
	lc_browser_fetch( w->b, source, NULL, BrowserChanged, w );
}

JNIEXPORT jint JNICALL JNI_FN( browserChanges )( JNIEnv *env, jclass cls, jlong h )
{
	(void)env; (void)cls;
	return __atomic_load_n( &((browser_t *)(intptr_t)h)->changes, __ATOMIC_SEQ_CST );
}

JNIEXPORT jboolean JNICALL JNI_FN( browserBusy )( JNIEnv *env, jclass cls, jlong h, jint source )
{
	(void)env; (void)cls;
	return lc_browser_is_busy( ((browser_t *)(intptr_t)h)->b, source ) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL JNI_FN( browserFetched )( JNIEnv *env, jclass cls, jlong h, jint source )
{
	(void)env; (void)cls;
	return lc_browser_fetched( ((browser_t *)(intptr_t)h)->b, source ) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL JNI_FN( browserStatus )( JNIEnv *env, jclass cls, jlong h, jint source )
{
	char copy[512];

	(void)cls;
	lc_strlcpy( copy, lc_browser_status_text( ((browser_t *)(intptr_t)h)->b, source ), sizeof( copy ) );
	return (*env)->NewStringUTF( env, copy );
}

/* a header tap (lc_column order: fav, name, lock, map, players, humans, type, cmod, ping) */
JNIEXPORT void JNICALL JNI_FN( browserSortClick )( JNIEnv *env, jclass cls, jlong h, jint column )
{
	browser_t *w = (browser_t *)(intptr_t)h;

	(void)env; (void)cls;
	w->nKeys = lc_sort_click( w->keys, w->nKeys, LC_COL_PING + 1, (lc_column)column );
}

/* the first sort key: column * 2 + (ascending ? 1 : 0), for the header's arrow */
JNIEXPORT jint JNICALL JNI_FN( browserSortKey )( JNIEnv *env, jclass cls, jlong h )
{
	browser_t *w = (browser_t *)(intptr_t)h;

	(void)env; (void)cls;
	return w->nKeys ? (jint)w->keys[0].column * 2 + ( w->keys[0].ascending ? 1 : 0 ) : -1;
}

/* Rows in display order, each one string of fields separated by \x1f:
   address, name (colour codes removed), colours (one digit 0-7 per name byte), map, map state
   (LC_MAP_*), players, maxPlayers, humans (-1 unknown), humansCounted, game, cMod text, ping,
   needPass, favorite, thisComputer */
JNIEXPORT jobjectArray JNICALL JNI_FN( browserRows )( JNIEnv *env, jclass cls, jlong h, jint source,
	jboolean onlyFav, jboolean onlyPeople, jobjectArray favorites )
{
	browser_t *w = (browser_t *)(intptr_t)h;
	int nFav = favorites ? (*env)->GetArrayLength( env, favorites ) : 0, i, k, n;
	const char **fav = calloc( nFav ? nFav : 1, sizeof( *fav ) );
	jstring *js = calloc( nFav ? nFav : 1, sizeof( *js ) );
	const lc_browser_row *rows = NULL;
	jobjectArray a;

	(void)cls;
	for ( i = 0; i < nFav; i++ ) {
		js[i] = (jstring)(*env)->GetObjectArrayElement( env, favorites, i );
		fav[i] = (*env)->GetStringUTFChars( env, js[i], NULL );
	}
	n = lc_browser_rows( w->b, source, onlyFav, onlyPeople, fav, nFav, w->keys, w->nKeys, &rows );
	a = StringArray( env, n );
	for ( i = 0; i < n; i++ ) {
		const lc_browser_row *r = &rows[i];
		char name[256], colors[256], game[64], cmod[64] = "", map[64], line[1024];
		unsigned char idx[256];
		size_t len;

		lc_decode_colors( r->info.hostname[0] ? r->info.hostname : r->info.name, name, sizeof( name ), idx );
		len = strlen( name );
		if ( !len ) {
			lc_strlcpy( name, r->info.address, sizeof( name ) );
			len = strlen( name );
			memset( idx, 7, len );
		}
		for ( k = 0; k < (int)len && k < (int)sizeof( colors ) - 1; k++ )
			colors[k] = (char)( '0' + ( idx[k] & 7 ) );
		colors[k] = 0;
		/* the control characters the separator uses can't be in a name, and servers send
		   Latin-1, not UTF-8 (JNI's NewStringUTF would abort on it): one byte stays one char */
		for ( k = 0; name[k]; k++ )
			if ( (unsigned char)name[k] < 32 )
				name[k] = ' ';
			else if ( (unsigned char)name[k] >= 128 )
				name[k] = '?';
		lc_strlcpy( map, r->info.map, sizeof( map ) );
		for ( k = 0; map[k]; k++ )
			if ( (unsigned char)map[k] < 32 || (unsigned char)map[k] >= 128 )
				map[k] = '?';
		lc_browser_gametype_name( r->info.gametype, game, sizeof( game ) );
		if ( r->isCmod && !lc_cmod_number( r->cmodVersion, cmod, sizeof( cmod ) ) )
			lc_strlcpy( cmod, "\xe2\x9c\x93", sizeof( cmod ) );
		lc_snprintf( line, sizeof( line ), "%s\x1f%s\x1f%s\x1f%s\x1f%d\x1f%d\x1f%d\x1f%d\x1f%d\x1f%s\x1f%s\x1f%d\x1f%d\x1f%d\x1f%d",
			r->info.address, name, colors, map, lc_map_state( w->maps, r->info.map ),
			r->info.players, r->info.maxPlayers, r->info.humans, r->info.humansCounted, game, cmod,
			r->info.ping, r->info.needPass, r->favorite, r->thisComputer );
		SetString( env, a, i, line );
	}
	for ( i = 0; i < nFav; i++ ) {
		(*env)->ReleaseStringUTFChars( env, js[i], fav[i] );
		(*env)->DeleteLocalRef( env, js[i] );
	}
	free( fav );
	free( js );
	return a;
}

/* ---- names and co-op --------------------------------------------------------------- */

JNIEXPORT jstring JNICALL JNI_FN( cleanName )( JNIEnv *env, jclass cls, jstring in )
{
	const char *s = (*env)->GetStringUTFChars( env, in, NULL );
	char out[64];

	(void)cls;
	lc_clean_name( s, out, sizeof( out ) );
	(*env)->ReleaseStringUTFChars( env, in, s );
	return (*env)->NewStringUTF( env, out );
}

JNIEXPORT jstring JNICALL JNI_FN( coopSafeName )( JNIEnv *env, jclass cls, jstring in )
{
	const char *s = (*env)->GetStringUTFChars( env, in, NULL );
	char out[64];

	(void)cls;
	lc_coop_safe_name( s, out, sizeof( out ) );
	(*env)->ReleaseStringUTFChars( env, in, s );
	return (*env)->NewStringUTF( env, out );
}

/* "id\x1fname\x1fgroup" for each character of coop_characters.h */
JNIEXPORT jobjectArray JNICALL JNI_FN( coopCharacters )( JNIEnv *env, jclass cls )
{
	int n = lc_coop_character_count( ), i;
	jobjectArray a = StringArray( env, n );

	(void)cls;
	for ( i = 0; i < n; i++ ) {
		const char *id = "", *name = "", *group = "";
		char line[256];

		lc_coop_character( i, &id, &name, &group );
		lc_snprintf( line, sizeof( line ), "%s\x1f%s\x1f%s", id, name, group );
		SetString( env, a, i, line );
	}
	return a;
}
