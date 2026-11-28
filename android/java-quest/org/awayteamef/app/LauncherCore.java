package org.awayteamef.app;

import android.util.Log;

/**
 * The desktop launchers' core (EFAndroid-SP/app/jni/efcode/launcher, plain C) through
 * liblauncher.so (android/jni/launcher_jni.c). The start screen uses it for what the desktop
 * launchers do the same way: the game files' checks (Expansion Pack), Choose / Add Game Files
 * (CD images, BaseEF .pk3s, GOG's installer: the same plans, checks, rollback and alert texts),
 * and the server browser (master query, getinfo/getstatus, sorting, filters, status lines).
 *
 * Handles are native pointers; free them with the matching *Free call.
 */
final class LauncherCore {
    private static final String TAG = "AwayTeamEF";
    static final boolean LOADED;

    static {
        boolean ok;
        try {
            System.loadLibrary("launcher");
            ok = true;
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "liblauncher.so missing: the start screen runs without its import and server browser", e);
            ok = false;
        }
        LOADED = ok;
    }

    private LauncherCore() {}

    /** The field separator of browserRows / coopCharacters. */
    static final String SEP = "\u001f";

    static native void nativeInit(String obbDir);

    // ---- game files
    static native boolean hasExpansion(String homeBase);
    static native boolean haveGog();

    /** A picked file's descriptor, served to the core under path (GameImport); cleared before the descriptors close. */
    static native void pickedAdd(String path, int fd);
    static native void pickedClear();
    /** A CD image (.iso/.bin/.cue/.img/.cdr) or GOG's setup .exe. 0: out[0..1] = the alert's title and text. */
    static native long planUnpack(String path, boolean onlyNew, String homeBase, String[] out);
    /** .pk3 files. 0: out = "Nothing to add". */
    static native long planAdd(String[] paths, String homeBase, String[] out);
    static native boolean jobCheckSpace(long job, String[] out);
    /** [0] progress panel title, [1] its label, [2] the number of files. */
    static native String[] jobTexts(long job);
    static native long jobTotal(long job);
    static native long jobProgress(long job);
    static native void jobCancel(long job);
    /** Blocking: 1 done, 0 failed (out = the alert), -1 cancelled. */
    static native int jobRun(long job, String[] out);
    static native void jobFree(long job);

    // ---- server browser
    static native long browserNew(String homeBase);
    static native void browserFree(long b);
    static native String[] browserSources(long b);
    static native void browserFetch(long b, int source);
    static native int browserChanges(long b);
    static native boolean browserBusy(long b, int source);
    static native boolean browserFetched(long b, int source);
    static native String browserStatus(long b, int source);
    static native void browserSortClick(long b, int column);
    static native int browserSortKey(long b);
    static native String[] browserRows(long b, int source, boolean onlyFav, boolean onlyPeople, String[] favorites);

    // ---- names and co-op
    static native String cleanName(String name);
    static native String coopSafeName(String name);
    static native String[] coopCharacters();

    // browser row fields
    static final int R_ADDRESS = 0, R_NAME = 1, R_COLOURS = 2, R_MAP = 3, R_MAPSTATE = 4, R_PLAYERS = 5,
            R_MAX = 6, R_HUMANS = 7, R_COUNTED = 8, R_GAME = 9, R_CMOD = 10, R_PING = 11, R_NEEDPASS = 12,
            R_FAVORITE = 13, R_THISCOMPUTER = 14;
    static final int MAP_HAVE = 0, MAP_MISSING = 1, MAP_EXPANSION = 2;
    // lc_column
    static final int COL_FAV = 0, COL_NAME = 1, COL_LOCK = 2, COL_MAP = 3, COL_PLAYERS = 4, COL_HUMANS = 5,
            COL_TYPE = 6, COL_CMOD = 7, COL_PING = 8;
}
