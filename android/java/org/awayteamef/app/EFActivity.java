package org.awayteamef.app;

import android.content.Intent;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.util.ArrayList;

/**
 * The game's activity: SDL 3's SDLActivity, which runs SDL_main() from libmain.so
 * on its own thread. This class only names the libraries and passes the command line.
 *
 * Engine arguments come from the launching intent's "args" string extra, split on
 * spaces, for example:
 *   adb shell am start -n org.awayteamef.app/.EFActivity -e args "+set s_volume 0"
 *
 * Game data: <obb dir>/baseEF/pak0.pk3 .. pak3.pk3, that is
 * /sdcard/Android/obb/org.awayteamef.app/baseEF/ (ANDROID_DATA_ROOT in android/CMakeLists.txt).
 */
public class EFActivity extends SDLActivity {
    private static final String TAG = "AwayTeamEF";

    @Override
    protected void onCreate(android.os.Bundle savedInstanceState) {
        // getObbDir() has the system create the app's OBB folder (an app can't make it
        // itself on Android 11+); the engine then reads and writes it by path
        java.io.File obb = getObbDir();
        if (obb != null) {
            new java.io.File(obb, "baseEF").mkdirs();
            Log.i(TAG, "data folder: " + obb + "/baseEF");
        }
        super.onCreate(savedInstanceState);
    }

    @Override
    protected String[] getLibraries() {
        // libSDL2.so is sdl2-compat, which the engine is built against; it finds
        // libSDL3.so (already loaded) at run time
        return new String[] { "SDL3", "SDL2", "main" };
    }

    @Override
    protected String[] getArguments() {
        ArrayList<String> args = new ArrayList<>();
        Intent intent = getIntent();
        String extra = intent != null ? intent.getStringExtra("args") : null;
        if (extra != null) {
            for (String a : extra.trim().split("\\s+")) {
                if (!a.isEmpty()) {
                    args.add(a);
                }
            }
        }
        Log.i(TAG, "engine arguments: " + args);
        return args.toArray(new String[0]);
    }
}
