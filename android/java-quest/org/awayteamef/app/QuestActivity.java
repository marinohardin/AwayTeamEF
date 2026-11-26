package org.awayteamef.app;

import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.WindowManager;

/**
 * The Quest build's activity (AndroidManifest-quest.xml; only compiled by make-apk.sh VR=1).
 * It is EFActivity plus two things:
 *  - it hands the JavaVM and itself to the vr module (nativeSetVRContext, in libmain.so),
 *    which OpenXR's Android loader and instance need;
 *  - it keeps the screen on and the system bars hidden.
 * Rendering and controllers are native (efcode/vr). With no OpenXR runtime the game stays flat.
 */
public class QuestActivity extends EFActivity {
    private static final String TAG = "AwayTeamEF";

    private native void nativeSetVRContext();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // SDLActivity.onCreate loads the libraries (getLibraries) and starts nothing native yet
        super.onCreate(savedInstanceState);
        try {
            nativeSetVRContext();
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "VR: libmain.so has no nativeSetVRContext (not a VR build?)", e);
        }
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        hideSystemUi();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            hideSystemUi();
        }
    }

    @SuppressWarnings("deprecation")
    private void hideSystemUi() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
    }
}
