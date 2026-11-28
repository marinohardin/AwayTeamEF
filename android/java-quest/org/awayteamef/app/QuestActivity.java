package org.awayteamef.app;

import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.res.AssetManager;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.WindowManager;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;

/**
 * The Quest build's activity (AndroidManifest-quest.xml; only compiled by make-apk.sh VR=1).
 * It is EFActivity plus these things:
 *  - it hands the JavaVM and itself to the vr module (nativeSetVRContext, in libmain.so),
 *    which OpenXR's Android loader and instance need;
 *  - it copies cMod's game modules (assets/cmod/*.pk3 in the APK, make-apk.sh CMOD_PAK) to
 *    files/cmod/ and tells the engine (nativeSetCmodDir; files.c checks them against cMod's
 *    release list, as on the desktop);
 *  - the game's own Quit comes back here as quitToLauncher() (android/jni/quest_jni.c), which
 *    shows the start screen again;
 *  - it keeps the screen on and the system bars hidden.
 * It runs in its own process (":game", AndroidManifest-quest.xml): the engine runs once per
 * process and ends it at quit, while the start screen stays in the main process.
 * Rendering and controllers are native (efcode/vr). With no OpenXR runtime the game stays flat.
 */
public class QuestActivity extends EFActivity {
    private static final String TAG = "AwayTeamEF";

    /** true in the start screen's intent when the player quit from the game's menus */
    public static final String EXTRA_FROM_GAME = "org.awayteamef.quest.FROM_GAME";

    private native void nativeSetVRContext();
    private native void nativeSetCmodDir(String dir);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        File cmod = copyCmodPaks();
        // SDLActivity.onCreate loads the libraries (getLibraries) and starts nothing native yet
        super.onCreate(savedInstanceState);
        try {
            nativeSetVRContext();
            nativeSetCmodDir(cmod != null ? cmod.getAbsolutePath() : null);
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

    /**
     * The game's own Quit (a bare "quit" from its menus), called by the engine on the game thread
     * right before the game process exits: show the start screen, which lives in the main
     * process, and close this activity.
     */
    public void quitToLauncher() {
        Intent i = new Intent(this, StartActivity.class);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        i.putExtra(EXTRA_FROM_GAME, true);
        try {
            startActivity(i);
        } catch (RuntimeException e) {
            Log.e(TAG, "quit: can't start the start screen", e);
        }
        finish();
    }

    /**
     * cMod's paks from the APK's assets/cmod/ into files/cmod/, when this install of the app hasn't
     * copied them yet (its own stamp: versionCode and lastUpdateTime); paks no longer in the APK
     * are removed. Returns the folder, or null when the APK has none.
     */
    private File copyCmodPaks() {
        File dir = new File(getFilesDir(), "cmod");
        AssetManager am = getAssets();
        String[] names;
        try {
            names = am.list("cmod");
        } catch (IOException e) {
            names = null;
        }
        Set<String> paks = new HashSet<>();
        if (names != null) {
            for (String n : names) {
                if (n.toLowerCase(java.util.Locale.ROOT).endsWith(".pk3")) {
                    paks.add(n);
                }
            }
        }
        String stamp = "";
        try {
            PackageInfo pi = getPackageManager().getPackageInfo(getPackageName(), 0);
            stamp = pi.getLongVersionCode() + " " + pi.lastUpdateTime + " " + new java.util.TreeSet<>(paks);
        } catch (Exception e) {
            Log.w(TAG, "cMod: no package info", e);
        }
        File stampFile = new File(dir, ".copied");
        if (stamp.equals(readSmall(stampFile))) {
            boolean all = true;
            for (String n : paks) {
                all &= new File(dir, n).isFile();
            }
            if (all) {
                return paks.isEmpty() ? null : dir;
            }
        }
        dir.mkdirs();
        File[] old = dir.listFiles();
        if (old != null) {
            for (File f : old) {
                if (!paks.contains(f.getName())) {
                    f.delete();
                }
            }
        }
        boolean ok = true;
        for (String n : paks) {
            File tmp = new File(dir, n + ".tmp");
            try (InputStream in = am.open("cmod/" + n); OutputStream out = new FileOutputStream(tmp)) {
                byte[] buf = new byte[65536];
                for (int r; (r = in.read(buf)) > 0; ) {
                    out.write(buf, 0, r);
                }
            } catch (IOException e) {
                Log.e(TAG, "cMod: can't copy " + n, e);
                tmp.delete();
                ok = false;
                continue;
            }
            if (!tmp.renameTo(new File(dir, n))) {
                tmp.delete();
                ok = false;
            }
        }
        if (ok) {
            try (OutputStream out = new FileOutputStream(stampFile)) {
                out.write(stamp.getBytes(StandardCharsets.UTF_8));
            } catch (IOException e) {
                Log.w(TAG, "cMod: can't write " + stampFile, e);
            }
        }
        Log.i(TAG, "cMod: " + (paks.isEmpty() ? "none in this APK" : "copied " + paks + " to " + dir));
        return paks.isEmpty() ? null : dir;
    }

    private static String readSmall(File f) {
        byte[] b = new byte[4096];
        try (InputStream in = new java.io.FileInputStream(f)) {
            int n = in.read(b);
            return n > 0 ? new String(Arrays.copyOf(b, n), StandardCharsets.UTF_8) : "";
        } catch (IOException e) {
            return null;
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
