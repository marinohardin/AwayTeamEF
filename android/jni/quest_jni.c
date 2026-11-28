/*
===========================================================================
AwayTeamEF for Quest: the Android shell's hand-over to the engine and the vr module.

This file is part of AwayTeamEF and is licensed under the GNU General
Public License, version 2 or (at your option) any later version.
===========================================================================

Only in the Quest build (android/CMakeLists.txt, EF_VR=ON, which also defines EF_QUEST).
QuestActivity.onCreate calls these once libmain.so is loaded, well before SDL_main starts
the engine:
  nativeSetVRContext(this)    the JavaVM and the activity, for the renderer's VR_Init
                              (xrInitializeLoaderKHR, XR_KHR_android_create_instance)
  nativeSetCmodDir(dir)       where QuestActivity copied cMod's paks from the APK
                              (files.c FS_LoadCmodPaks: Sys_QuestCmodDir)
and the engine calls back:
  Sys_PlatformQuitToLauncher  the game's own Quit (common.c): back at the start screen
  Sys_QuestPublicAddress      the Loopback Host Fix's public address (cl_main.c)
*/

#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <android/log.h>

#include "vr.h"
#include "lc_nat.h"		// the launchers' router lookup (efcode/launcher)

#define TAG "AwayTeamEF"

static JavaVM *quest_vm;
static jobject quest_activity;	// a global reference: the activity outlives this call
static char quest_cmodDir[1024];

static void Quest_LcLog( const char *line, void *user ) {
	(void)user;
	__android_log_print( ANDROID_LOG_INFO, TAG, "%s", line );
}

JNIEXPORT void JNICALL Java_org_awayteamef_app_QuestActivity_nativeSetVRContext( JNIEnv *env, jobject thiz ) {
	JavaVM *vm = NULL;

	if ( ( *env )->GetJavaVM( env, &vm ) != JNI_OK || !vm ) {
		__android_log_print( ANDROID_LOG_ERROR, TAG, "VR: no JavaVM; VR stays off" );
		return;
	}
	if ( quest_activity ) {
		( *env )->DeleteGlobalRef( env, quest_activity );
	}
	quest_vm = vm;
	quest_activity = ( *env )->NewGlobalRef( env, thiz );
	VR_SetAndroidContext( (void *)vm, (void *)quest_activity );
	__android_log_print( ANDROID_LOG_INFO, TAG, "VR: Android context set" );

	// the Loopback Host Fix (cl_main.c) needs this network's public address, which the desktop
	// launchers ask the router for when they start (NAT-PMP, then UPnP: lc_nat.c). The Quest's
	// start screen runs in another process, so the game asks now, in the background; silent
	// when the router doesn't answer. "+set cl_loopbackHostIP <ip>" still overrides it.
	lc_set_log_sink( Quest_LcLog, NULL );
	lc_public_address_start();
}

JNIEXPORT void JNICALL Java_org_awayteamef_app_QuestActivity_nativeSetCmodDir( JNIEnv *env, jobject thiz, jstring dir ) {
	const char *s;

	(void)thiz;
	quest_cmodDir[0] = 0;
	if ( !dir || !( s = ( *env )->GetStringUTFChars( env, dir, NULL ) ) ) {
		return;
	}
	if ( strlen( s ) < sizeof( quest_cmodDir ) ) {
		strcpy( quest_cmodDir, s );
	}
	( *env )->ReleaseStringUTFChars( env, dir, s );
}

// files.c: the folder of the bundled cMod paks, NULL when there are none
const char *Sys_QuestCmodDir( void ) {
	return quest_cmodDir[0] ? quest_cmodDir : NULL;
}

// cl_main.c: this network's public address from the router, NULL while unknown
const char *Sys_QuestPublicAddress( void ) {
	return lc_public_address();
}

/*
The game's own Quit: Com_Quit_f calls this for a bare "quit" (the menus' Quit), before it shuts
the engine down. It only arms an atexit handler; that one runs in exit(), after the shutdown,
and has QuestActivity start the start screen and finish itself. The game process then ends as
always; the start screen lives in the app's main process (QuestActivity runs in ":game",
AndroidManifest-quest.xml), so it stays.

Not for a quit the player didn't pick in the game: the headset's system menu ends the XR
session (VR_GetExitReason; the renderer then runs a "quit" of its own), and the system closing
the activity comes as "quit Closed window", which never comes here. Both end the app as before.
*/
static int quest_quitToLauncherArmed;

static void Quest_AtExit( void ) {
	JNIEnv *env = NULL;
	jclass cls;
	jmethodID mid;
	int attached = 0;

	if ( !quest_quitToLauncherArmed || !quest_vm || !quest_activity ) {
		return;
	}
	quest_quitToLauncherArmed = 0;
	if ( ( *quest_vm )->GetEnv( quest_vm, (void **)&env, JNI_VERSION_1_6 ) != JNI_OK ) {
		if ( ( *quest_vm )->AttachCurrentThread( quest_vm, &env, NULL ) != JNI_OK ) {
			return;
		}
		attached = 1;
	}
	cls = ( *env )->GetObjectClass( env, quest_activity );
	mid = cls ? ( *env )->GetMethodID( env, cls, "quitToLauncher", "()V" ) : NULL;
	if ( mid ) {
		__android_log_print( ANDROID_LOG_INFO, TAG, "quit from the game: back to the start screen" );
		( *env )->CallVoidMethod( env, quest_activity, mid );
	}
	if ( ( *env )->ExceptionCheck( env ) ) {
		( *env )->ExceptionClear( env );
	}
	if ( attached ) {
		( *quest_vm )->DetachCurrentThread( quest_vm );
	}
}

void Sys_PlatformQuitToLauncher( void ) {
	static int registered;

	if ( VR_GetExitReason() != VR_EXIT_NONE ) {
		return;		// the headset's system menu (or a lost headset) ended the session: quit the app
	}
	quest_quitToLauncherArmed = 1;
	if ( !registered ) {
		registered = 1;
		atexit( Quest_AtExit );
	}
}
