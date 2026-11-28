package org.awayteamef.app;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.system.Os;
import android.util.Log;

import java.io.Closeable;
import java.io.File;
import java.util.ArrayList;
import java.util.List;

/**
 * Hands files picked with Android's file picker (content:// URIs, not paths) to the launcher core,
 * which reads plain paths (sys_iso.c, sys_osx_gog.c and the copy job use stdio and open()).
 *
 * Each picked file is opened read-only here; the core gets a path in the app's cache folder named like
 * the picked file (the core tells a CD image or an installer by its extension, and a .cue names its
 * .bin next to it). The path is registered with liblauncher, which serves the core's fopen/open/stat
 * of it from the open descriptor (reopening /proc/self/fd/<n> of shared storage is refused). A
 * symbolic link to /proc/self/fd/<n> is left at the path too, so it exists for anything else that looks.
 * The descriptors stay open until the copy job ends (close()).
 */
final class GameImport implements Closeable {
    private static final String TAG = "AwayTeamEF";

    final List<String> paths = new ArrayList<String>();
    final List<String> names = new ArrayList<String>();
    private final List<ParcelFileDescriptor> fds = new ArrayList<ParcelFileDescriptor>();
    private final File dir;

    private GameImport(Context ctx) {
        dir = new File(ctx.getCacheDir(), "picked");
        clear(dir);
        dir.mkdirs();
    }

    private static void clear(File d) {
        File[] fs = d.listFiles();
        if (fs != null) for (File f : fs) f.delete();
    }

    static String displayName(ContentResolver cr, Uri u) {
        try (Cursor c = cr.query(u, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getString(0);
        } catch (Exception e) {
            Log.w(TAG, "no name for " + u + ": " + e);
        }
        String s = u.getLastPathSegment();
        return s == null ? "picked" : s.substring(s.lastIndexOf('/') + 1);
    }

    private void add(ContentResolver cr, Uri u, String name) throws Exception {
        // only the file's own name: no folders, nothing hidden
        name = name.replace('/', '_');
        if (name.startsWith(".")) name = "_" + name.substring(1);
        for (String n : names) {
            if (n.equalsIgnoreCase(name)) return;       // picked twice
        }
        ParcelFileDescriptor pfd = cr.openFileDescriptor(u, "r");
        if (pfd == null) throw new Exception("can't open " + name);
        fds.add(pfd);
        File link = new File(dir, name);
        Os.symlink("/proc/self/fd/" + pfd.getFd(), link.getPath());
        LauncherCore.pickedAdd(link.getPath(), pfd.getFd());
        paths.add(link.getPath());
        names.add(name);
        Log.i(TAG, "import: picked " + name);
    }

    /** Files picked with ACTION_OPEN_DOCUMENT (a disc image, its .cue, GOG's installer, .pk3s). */
    static GameImport files(Context ctx, List<Uri> uris) throws Exception {
        GameImport g = new GameImport(ctx);
        ContentResolver cr = ctx.getContentResolver();
        try {
            for (Uri u : uris) g.add(cr, u, displayName(cr, u));
        } catch (Exception e) {
            g.close();
            throw e;
        }
        return g;
    }

    /**
     * A folder picked with ACTION_OPEN_DOCUMENT_TREE: its BaseEF child (any case) if it has one,
     * else the folder itself; its top-level .pk3 files (as the desktop launchers' chooser).
     */
    static GameImport folder(Context ctx, Uri tree) throws Exception {
        GameImport g = new GameImport(ctx);
        ContentResolver cr = ctx.getContentResolver();
        try {
            String dirId = DocumentsContract.getTreeDocumentId(tree);
            String[][] kids = list(cr, tree, dirId);
            for (String[] k : kids) {
                if (k[1].equalsIgnoreCase("baseEF") && DocumentsContract.Document.MIME_TYPE_DIR.equals(k[2])) {
                    dirId = k[0];
                    kids = list(cr, tree, dirId);
                    break;
                }
            }
            for (String[] k : kids) {
                if (k[1].toLowerCase().endsWith(".pk3") && !DocumentsContract.Document.MIME_TYPE_DIR.equals(k[2]))
                    g.add(cr, DocumentsContract.buildDocumentUriUsingTree(tree, k[0]), k[1]);
            }
        } catch (Exception e) {
            g.close();
            throw e;
        }
        return g;
    }

    private static String[][] list(ContentResolver cr, Uri tree, String id) {
        List<String[]> out = new ArrayList<String[]>();
        Cursor c = cr.query(DocumentsContract.buildChildDocumentsUriUsingTree(tree, id), new String[]{
                DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE}, null, null, null);
        if (c != null) {
            try {
                while (c.moveToNext()) out.add(new String[]{c.getString(0), c.getString(1), c.getString(2)});
            } finally {
                c.close();
            }
        }
        return out.toArray(new String[0][]);
    }

    /** The one path to unpack: a .cue if one was picked (it names its .bin), else the first image or installer. */
    String unpackPath() {
        for (String p : paths) if (p.toLowerCase().endsWith(".cue")) return p;
        for (String p : paths) {
            String l = p.toLowerCase();
            if (l.endsWith(".iso") || l.endsWith(".bin") || l.endsWith(".img") || l.endsWith(".cdr") || l.endsWith(".exe"))
                return p;
        }
        return paths.isEmpty() ? null : paths.get(0);
    }

    @Override public void close() {
        if (LauncherCore.LOADED) LauncherCore.pickedClear();
        for (ParcelFileDescriptor p : fds) {
            try { p.close(); } catch (Exception ignored) { }
        }
        fds.clear();
        clear(dir);
    }
}
