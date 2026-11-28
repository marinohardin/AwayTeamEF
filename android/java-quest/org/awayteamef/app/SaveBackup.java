package org.awayteamef.app;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.HashMap;
import java.util.Map;

/**
 * Saves and settings that survive uninstalling.
 *
 * The engine keeps everything in the app's OBB folder (/sdcard/Android/obb/org.awayteamef.quest):
 * the copied game files (baseEF/*.pk3), the saves and the configs. Android deletes that folder when
 * the app is uninstalled, which is right for the .pk3 copies and wrong for the saves.
 *
 * So the start screen copies everything there except .pk3 files (saves, efspconfig.cfg, hmconfig.cfg,
 * the co-op and other cfgs, screenshots) into a folder the player picks once with Android's folder
 * picker (Storage Access Framework; suggested: Documents/AwayTeamEF). It does that each time the
 * start screen comes back from the game, and on Back Up Now. Only changed files are copied.
 *
 * On Android 11 and later a reinstalled app can't read files its previous install wrote to shared
 * storage, except through that picker; so after a reinstall "Restore…" opens the picker on the same
 * folder, and the files are copied back into the OBB folder. Nothing needs All files access.
 */
final class SaveBackup {
    private static final String TAG = "AwayTeamEF";
    static final String PREFS = "backup";
    static final String KEY_TREE = "tree";
    static final String KEY_TIME = "time";
    static final String KEY_RESULT = "result";
    private static final long MAX_FILE = 64L << 20;     // a backup is saves and settings, not data

    private SaveBackup() {}

    /** Not backed up: the game data (copied .pk3s, also maps a server sent), unfinished copies, logs. */
    static boolean skip(String name) {
        String n = name.toLowerCase();
        return n.endsWith(".pk3") || n.endsWith(".part") || n.endsWith(".log") || n.startsWith(".")
                || n.endsWith(".tmp");
    }

    static Uri treeDoc(Uri tree) {
        return DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
    }

    private static final class Child {
        String id, mime;
        long size, modified;
    }

    private static Map<String, Child> children(ContentResolver cr, Uri tree, String parentId) {
        Map<String, Child> m = new HashMap<String, Child>();
        Uri u = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parentId);
        Cursor c = cr.query(u, new String[]{
                DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE,
                DocumentsContract.Document.COLUMN_LAST_MODIFIED}, null, null, null);
        if (c == null) return m;
        try {
            while (c.moveToNext()) {
                Child ch = new Child();
                ch.id = c.getString(0);
                ch.mime = c.getString(2);
                ch.size = c.isNull(3) ? -1 : c.getLong(3);
                ch.modified = c.isNull(4) ? 0 : c.getLong(4);
                m.put(c.getString(1), ch);
            }
        } finally {
            c.close();
        }
        return m;
    }

    static final class Result {
        int copied, unchanged, failed;
        String error;

        @Override public String toString() {
            return copied + " copied, " + unchanged + " unchanged" + (failed > 0 ? ", " + failed + " failed" : "");
        }
    }

    /** Copies obb (minus .pk3s etc.) into the tree. Blocking: call it on a worker thread. */
    static Result backup(Context ctx, Uri tree, File obb) {
        Result r = new Result();
        try {
            copyOut(ctx.getContentResolver(), tree, DocumentsContract.getTreeDocumentId(tree), obb, r);
        } catch (Exception e) {
            r.error = e.getMessage() == null ? e.toString() : e.getMessage();
            Log.w(TAG, "backup failed", e);
        }
        Log.i(TAG, "backup to " + tree + ": " + r + (r.error != null ? " (" + r.error + ")" : ""));
        return r;
    }

    private static void copyOut(ContentResolver cr, Uri tree, String dirId, File dir, Result r) throws IOException {
        File[] files = dir.listFiles();
        if (files == null) return;
        Map<String, Child> have = children(cr, tree, dirId);
        for (File f : files) {
            if (skip(f.getName())) continue;
            Child c = have.get(f.getName());
            if (f.isDirectory()) {
                String id;
                if (c != null && DocumentsContract.Document.MIME_TYPE_DIR.equals(c.mime)) {
                    id = c.id;
                } else {
                    Uri made = DocumentsContract.createDocument(cr, DocumentsContract.buildDocumentUriUsingTree(tree, dirId),
                            DocumentsContract.Document.MIME_TYPE_DIR, f.getName());
                    if (made == null) { r.failed++; continue; }
                    id = DocumentsContract.getDocumentId(made);
                }
                copyOut(cr, tree, id, f, r);
                continue;
            }
            if (!f.isFile() || f.length() > MAX_FILE) continue;
            if (c != null && c.size == f.length() && c.modified >= f.lastModified()) {
                r.unchanged++;
                continue;
            }
            Uri doc;
            if (c != null) {
                doc = DocumentsContract.buildDocumentUriUsingTree(tree, c.id);
            } else {
                doc = DocumentsContract.createDocument(cr, DocumentsContract.buildDocumentUriUsingTree(tree, dirId),
                        "application/octet-stream", f.getName());
                if (doc == null) { r.failed++; continue; }
            }
            try (InputStream in = new FileInputStream(f); OutputStream out = cr.openOutputStream(doc, "wt")) {
                if (out == null) throw new IOException("can't write " + f.getName());
                pump(in, out);
                r.copied++;
            } catch (IOException e) {
                r.failed++;
                Log.w(TAG, "backup: " + f + ": " + e);
            }
        }
    }

    /** Copies the tree's files (never .pk3s) into obb, replacing files of the same name. Blocking. */
    static Result restore(Context ctx, Uri tree, File obb) {
        Result r = new Result();
        try {
            copyIn(ctx.getContentResolver(), tree, DocumentsContract.getTreeDocumentId(tree), obb, r);
        } catch (Exception e) {
            r.error = e.getMessage() == null ? e.toString() : e.getMessage();
            Log.w(TAG, "restore failed", e);
        }
        Log.i(TAG, "restore from " + tree + ": " + r + (r.error != null ? " (" + r.error + ")" : ""));
        return r;
    }

    private static void copyIn(ContentResolver cr, Uri tree, String dirId, File dir, Result r) throws IOException {
        for (Map.Entry<String, Child> e : children(cr, tree, dirId).entrySet()) {
            String name = e.getKey();
            Child c = e.getValue();
            if (name == null || skip(name) || name.contains("/")) continue;
            File to = new File(dir, name);
            if (DocumentsContract.Document.MIME_TYPE_DIR.equals(c.mime)) {
                if (!to.isDirectory() && !to.mkdirs()) { r.failed++; continue; }
                copyIn(cr, tree, c.id, to, r);
                continue;
            }
            File part = new File(dir, name + ".part");
            try (InputStream in = cr.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, c.id));
                 OutputStream out = new FileOutputStream(part)) {
                if (in == null) throw new IOException("can't read " + name);
                pump(in, out);
            } catch (IOException ex) {
                part.delete();
                r.failed++;
                Log.w(TAG, "restore: " + name + ": " + ex);
                continue;
            }
            if (!part.renameTo(to)) {
                part.delete();
                r.failed++;
                continue;
            }
            r.copied++;
        }
    }

    private static void pump(InputStream in, OutputStream out) throws IOException {
        byte[] buf = new byte[1 << 16];
        int n;
        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
    }

    /** "Documents/AwayTeamEF" from a tree uri of the external storage provider, else its last part. */
    static String displayName(Uri tree) {
        try {
            String id = DocumentsContract.getTreeDocumentId(tree);
            int colon = id.indexOf(':');
            String path = colon >= 0 ? id.substring(colon + 1) : id;
            return path.isEmpty() ? "the storage's top folder" : path;
        } catch (Exception e) {
            return tree.toString();
        }
    }
}
