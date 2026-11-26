/*
===========================================================================
AwayTeamEF for Quest: the Android shell's hand-over to the vr module.

This file is part of AwayTeamEF and is licensed under the GNU General
Public License, version 2 or (at your option) any later version.
===========================================================================

Only in the Quest build (android/CMakeLists.txt, EF_VR=ON). QuestActivity.onCreate calls
nativeSetVRContext(this) once libmain.so is loaded, well before SDL_main starts the
renderer, whose VR_Init needs the JavaVM and the activity for xrInitializeLoaderKHR and
XR_KHR_android_create_instance.
*/

#include <jni.h>
#include <android/log.h>

#include "vr.h"

static jobject quest_activity;	// a global reference: the activity outlives this call

JNIEXPORT void JNICALL Java_org_awayteamef_app_QuestActivity_nativeSetVRContext( JNIEnv *env, jobject thiz ) {
	JavaVM *vm = NULL;

	if ( ( *env )->GetJavaVM( env, &vm ) != JNI_OK || !vm ) {
		__android_log_print( ANDROID_LOG_ERROR, "AwayTeamEF", "VR: no JavaVM; VR stays off" );
		return;
	}
	if ( quest_activity ) {
		( *env )->DeleteGlobalRef( env, quest_activity );
	}
	quest_activity = ( *env )->NewGlobalRef( env, thiz );
	VR_SetAndroidContext( (void *)vm, (void *)quest_activity );
	__android_log_print( ANDROID_LOG_INFO, "AwayTeamEF", "VR: Android context set" );
}
