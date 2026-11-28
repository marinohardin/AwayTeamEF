package org.awayteamef.app;

import android.app.Activity;
import android.content.ClipData;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.UriPermission;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.DocumentsContract;
import android.text.format.DateUtils;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * The Quest build's launcher (AndroidManifest-quest.xml; only compiled by make-apk.sh VR=1): a plain
 * Android activity, a flat panel in the headset, in the app's main process. It starts QuestActivity
 * (the game, process ":game") with the engine arguments the desktop launchers put on the command line
 * (efcode/launcher/lc_launch.c, docs/launcher-spec/launcher-data-coop.md §3.2, §6.2):
 *
 *   Single Player       +set com_holomatch 0
 *   Co-op: Host         +set com_holomatch 0 +set sp_coop 1 +set coop_name "<name>"
 *   Co-op: Join         +set com_holomatch 0 +set coop_connect "<ip:port>" +set coop_sex <male|female>
 *                       [+set coop_look <id>] +set coop_name "<name>" [+set coop_password "<pw>"]
 *   Play Holomatch      +set com_holomatch 1 +set name "<name>"
 *   a server (Browse)   +set com_holomatch 1 +set name "<name>" [+set password "<pw>"] +connect <ip:port>
 *
 * Its pages: the start screen (header, the modes, the player name, the game files, saves & settings),
 * Join a Co-op Game (number pad, LAN search, Munro and character, password), Browse Servers
 * (BrowserPage), Game Files (CD image, BaseEF folder, GOG installer: GameImport and the core's copy
 * jobs), and on-panel keyboards (TextEntry). No page opens the Quest's system keyboard.
 *
 * When the game's own Quit brings it back (QuestActivity.EXTRA_FROM_GAME) it refreshes the game
 * files' status and backs up the saves and settings (SaveBackup).
 */
public class StartActivity extends Activity {
    private static final String TAG = "AwayTeamEF";
    private static final String PREFS = "start";
    private static final String PREF_ADDRESS = "address";
    private static final String PREF_NAME = "name";
    private static final String PREF_SEX = "coopSex";
    private static final String PREF_LOOK = "coopLook";
    private static final int MAX_ADDRESS = 21;     // 255.255.255.255:65535
    private static final int MAX_NAME = 31;
    private static final int PICK_IMAGE = 1, PICK_FOLDER = 2, PICK_INSTALLER = 3, PICK_BACKUP = 4, PICK_RESTORE = 5;

    private Ui ui;
    private SharedPreferences prefs, backupPrefs;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private ScrollView pageHolder;
    private FrameLayout overlay;
    private String page = "main";
    private BrowserPage browser;
    private File obb, homeBase;
    private boolean backingUp;
    private boolean restoring;
    private long lastBackupStart;
    private boolean importing;

    // the co-op join page's state
    private final StringBuilder address = new StringBuilder();
    private String coopPassword = "";
    private TextView addressView, searchStatus;
    private TextView searchButton;
    private LinearLayout results;
    private volatile boolean searching;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Log.i(TAG, "StartActivity onCreate" + (fromGame(getIntent()) ? " (back from the game)" : ""));
        ui = new Ui(this);
        prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        backupPrefs = getSharedPreferences(SaveBackup.PREFS, MODE_PRIVATE);
        // getObbDir() has the system create the OBB folder (an app can't make it on Android 11+)
        obb = getObbDir();
        homeBase = new File(obb, "baseEF");
        homeBase.mkdirs();
        if (LauncherCore.LOADED) LauncherCore.nativeInit(obb.getPath());
        address.append(prefs.getString(PREF_ADDRESS, ""));

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Ui.BG);
        pageHolder = new ScrollView(this);
        pageHolder.setFillViewport(true);
        root.addView(pageHolder, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        overlay = new FrameLayout(this);
        overlay.setVisibility(View.GONE);
        overlay.setBackgroundColor(Color.argb(150, 0, 0, 0));
        overlay.setClickable(true);       // the page under a dialog takes no taps
        root.addView(overlay, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        setContentView(root);
    }

    private static boolean fromGame(Intent i) {
        return i != null && i.getBooleanExtra(QuestActivity.EXTRA_FROM_GAME, false);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        Log.i(TAG, "StartActivity onNewIntent" + (fromGame(intent) ? " (back from the game)" : ""));
        if (fromGame(intent)) showMain();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (page.equals("main")) showMain();
        autoBackup(fromGame(getIntent()));
    }

    @Override
    protected void onDestroy() {
        if (browser != null) browser.close();
        super.onDestroy();
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        if (overlay.getVisibility() == View.VISIBLE && !importing) {
            hideDialog();
        } else if (!page.equals("main")) {
            showMain();
        } else {
            super.onBackPressed();
        }
    }

    // ---------------------------------------------------------------- pages

    private void setPage(String name, View content) {
        if (browser != null && !name.equals("browser")) {
            browser.close();
            browser = null;
        }
        page = name;
        LinearLayout wrap = ui.column();
        wrap.setPadding(ui.dp(36), ui.dp(24), ui.dp(36), ui.dp(24));
        wrap.addView(content, Ui.matchWrap());
        pageHolder.removeAllViews();
        pageHolder.addView(wrap, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        pageHolder.scrollTo(0, 0);
    }

    private View header(String subtitle) {
        LinearLayout h = ui.row();
        h.addView(ui.icon(72));
        LinearLayout t = ui.column();
        t.setPadding(ui.dp(18), 0, 0, 0);
        // the desktop launchers' lettering (tools/launcher-titles/png), tinted as they tint it
        t.addView(ui.title("star_trek_voyager.png", Ui.SECONDARY, 1.5f, "Star Trek: Voyager"));
        View ef = ui.title("elite_force.png", Ui.TEXT, 1.5f, "Elite Force");
        ((LinearLayout.LayoutParams) ef.getLayoutParams()).topMargin = ui.dp(8);
        t.addView(ef);
        TextView sub = ui.text(subtitle, 19, Ui.SECONDARY);
        sub.setPadding(0, ui.dp(8), 0, 0);
        t.addView(sub);
        h.addView(t, Ui.weight(1));
        h.addView(ui.title("away_team_ef.png", Ui.GOLD, 1.5f, "AwayTeamEF"));
        return h;
    }

    private View topBar(String title) {
        LinearLayout top = ui.row();
        top.addView(ui.button("‹ Back", false, new View.OnClickListener() {
            @Override public void onClick(View v) { showMain(); }
        }));
        TextView t = ui.bold(title, 28, Ui.TEXT);
        t.setPadding(ui.dp(18), 0, ui.dp(12), 0);
        top.addView(t, Ui.weight(1));
        top.addView(ui.title("away_team_ef.png", Ui.GOLD, 1.1f, "AwayTeamEF"));
        return top;
    }

    // ---------------------------------------------------------------- game files

    /** What the start screen knows about baseEF in the OBB folder. */
    private static final class Data {
        boolean[] pak = new boolean[4];
        int extra;                    // other .pk3 files (maps, playermaps)
        boolean expansion;
        boolean canPlay() { return pak[0] && pak[1] && pak[2]; }
        String missingPatch() {
            if (pak[1] && pak[2]) return "";
            if (!pak[1] && !pak[2]) return "pak1.pk3 and pak2.pk3";
            return pak[1] ? "pak2.pk3" : "pak1.pk3";
        }
    }

    private Data data() {
        Data d = new Data();
        String[] names = homeBase.list();
        if (names != null) {
            for (String n : names) {
                String l = n.toLowerCase(Locale.ROOT);
                if (!l.endsWith(".pk3")) continue;
                boolean core = false;
                for (int i = 0; i < 4; i++) {
                    if (l.equals("pak" + i + ".pk3") && new File(homeBase, n).isFile()) {
                        d.pak[i] = true;
                        core = true;
                    }
                }
                if (!core) d.extra++;
            }
        }
        // the desktop's test (Virtual Voyager's first map in a .pk3); without the core: pak3
        d.expansion = LauncherCore.LOADED ? LauncherCore.hasExpansion(homeBase.getPath()) : d.pak[3];
        return d;
    }

    // ---------------------------------------------------------------- the start screen

    private void showMain() {
        final Data data = data();
        boolean play = data.canPlay();
        LinearLayout p = ui.column();
        p.addView(header("Choose how you want to play."));

        LinearLayout cols = ui.row();
        cols.setGravity(Gravity.TOP);
        LinearLayout left = ui.column(), right = ui.column();
        cols.addView(left, Ui.weight(1.15f));
        cols.addView(right, ui.gapLeft(Ui.weight(1), 26));
        p.addView(cols, ui.gapTop(Ui.matchWrap(), 18));

        modeCard(left, "👤", "Single Player", "The Voyager campaign, your saves and the Virtual Voyager.", play,
                new String[]{"Play"}, new Runnable[]{new Runnable() {
                    @Override public void run() { launch("+set com_holomatch 0"); }
                }});
        modeCard(left, "👥", "Co-op",
                "Play the campaign with a friend. The host is player 1 and plays the story; the friend joins as player 2.", play,
                new String[]{"Host", "Join…"}, new Runnable[]{new Runnable() {
                    @Override public void run() {
                        launch("+set com_holomatch 0 +set sp_coop 1 +set coop_name " + q(coopName()));
                    }
                }, new Runnable() {
                    @Override public void run() { showCoopJoin(); }
                }});
        modeCard(left, "🌐", "Holomatch", "Join games on your network or the internet, or play with bots.", play,
                new String[]{"Browse Servers", "Play Holomatch"}, new Runnable[]{new Runnable() {
                    @Override public void run() { showBrowser(); }
                }, new Runnable() {
                    @Override public void run() { launch("+set com_holomatch 1 +set name " + q(playerName())); }
                }});
        if (!play) {
            TextView why = ui.text(data.pak[0] ? "Add the 1.2 patch files first (see Game Files)." : "Add the game files first (see Game Files).",
                    17, Ui.ORANGE);
            why.setPadding(ui.dp(4), 0, 0, 0);
            left.addView(why);
        }

        // PLAYER: one name for Holomatch and co-op
        LinearLayout pl = ui.group(right, "Player");
        LinearLayout nameRow = ui.row();
        nameRow.addView(ui.text("Player name:", 19, Color.rgb(209, 209, 214)));
        TextView nameField = ui.text("", 22, Ui.TEXT);
        nameField.setText(Ui.colourText(playerName()));
        nameField.setGravity(Gravity.CENTER_VERTICAL);
        nameField.setMinHeight(ui.dp(58));
        nameField.setPadding(ui.dp(16), ui.dp(8), ui.dp(16), ui.dp(8));
        nameField.setBackground(fieldBackground());
        nameField.setClickable(true);
        nameField.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { editName(); }
        });
        nameRow.addView(nameField, ui.gapLeft(Ui.weight(1), 12));
        nameRow.addView(ui.smallButton("Change", new View.OnClickListener() {
            @Override public void onClick(View v) { editName(); }
        }), ui.gapLeft(Ui.wrap(), 10));
        pl.addView(nameRow);
        TextView nh = ui.text("Shown to other players in Holomatch and co-op.", 16, Ui.SECONDARY);
        nh.setPadding(0, ui.dp(6), 0, 0);
        pl.addView(nh);

        // GAME FILES
        LinearLayout gf = ui.group(right, "Game Files");
        gameFilesStatus(gf, data, true);

        // SAVES & SETTINGS
        LinearLayout sv = ui.group(right, "Saves & Settings");
        savesGroup(sv);

        LinearLayout foot = ui.row();
        foot.addView(ui.text("AwayTeamEF for Meta Quest " + version() + " · Made by Marino Hardin", 16, Ui.SECONDARY), Ui.weight(1));
        foot.addView(ui.smallButton("Quit", new View.OnClickListener() {
            @Override public void onClick(View v) { finishAndRemoveTask(); }
        }), new LinearLayout.LayoutParams(ui.dp(140), ViewGroup.LayoutParams.WRAP_CONTENT));
        p.addView(foot, ui.gapTop(Ui.matchWrap(), 10));
        setPage("main", p);
    }

    private android.graphics.drawable.Drawable fieldBackground() {
        android.graphics.drawable.StateListDrawable s = new android.graphics.drawable.StateListDrawable();
        s.addState(new int[]{android.R.attr.state_hovered}, Ui.round(Color.rgb(28, 28, 32), ui.dp(12), ui.dp(2), Ui.ACCENT));
        s.addState(new int[]{}, Ui.round(Ui.FIELD, ui.dp(12), ui.dp(1.5f), Color.rgb(72, 72, 74)));
        return s;
    }

    private void modeCard(LinearLayout parent, String icon, String title, String detail, boolean enabled,
                          String[] buttons, final Runnable[] actions) {
        LinearLayout c = ui.card(parent);
        TextView ic = ui.text(icon, 30, Ui.SECONDARY);
        ic.setGravity(Gravity.CENTER);
        c.addView(ic, new LinearLayout.LayoutParams(ui.dp(46), ViewGroup.LayoutParams.WRAP_CONTENT));
        LinearLayout t = ui.column();
        t.addView(ui.bold(title, 24, Ui.TEXT));
        TextView dt = ui.text(detail, 17, Ui.SECONDARY);
        dt.setPadding(0, ui.dp(4), 0, 0);
        t.addView(dt);
        c.addView(t, ui.gapLeft(Ui.weight(1), 12));
        LinearLayout bc = ui.column();
        for (int i = 0; i < buttons.length; i++) {
            final Runnable r = actions[i];
            TextView b = ui.button(buttons[i], i == 0 && buttons.length == 1, new View.OnClickListener() {
                @Override public void onClick(View v) { r.run(); }
            });
            b.setEnabled(enabled);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ui.dp(210), ViewGroup.LayoutParams.WRAP_CONTENT);
            if (i > 0) lp.topMargin = ui.dp(10);
            bc.addView(b, lp);
        }
        c.addView(bc, ui.gapLeft(Ui.wrap(), 14));
    }

    /** The Game Files group (start screen) or the status block of the Game Files page. */
    private void gameFilesStatus(LinearLayout g, Data data, boolean withButton) {
        if (!data.pak[0]) {
            g.addView(ui.bold("No game files yet", 19, Ui.TEXT));
            g.addView(ui.status("⚠", Ui.ORANGE,
                    "Choose an image of the game's CD (.iso, or .bin with its .cue), a BaseEF folder, or GOG's setup .exe."));
        } else {
            g.addView(ui.text("Android/obb/" + getPackageName() + "/baseEF", 17, Ui.TEXT));
            String miss = data.missingPatch();
            if (!miss.isEmpty()) {
                g.addView(ui.status("⚠", Ui.ORANGE, "Patch 1.2 missing · add " + miss + " with Add Game Files…"));
                TextView more = ui.text("These game files are from the original CD (version 1.0). The game also needs " + miss
                        + " from the 1.1 and 1.2 patches: the Expansion Pack's CD or its image brings them, or a patched installation (GOG, Steam, or a PC with patch 1.2).",
                        15, Ui.SECONDARY);
                more.setPadding(ui.dp(26), ui.dp(2), 0, 0);
                g.addView(more);
            } else {
                g.addView(ui.status("✔", Ui.GREEN, "Ready · copied into the app's own folder"));
            }
        }
        // the paks, one chip each
        LinearLayout chips = ui.row();
        chips.setPadding(0, ui.dp(10), 0, 0);
        for (int i = 0; i < 4; i++) {
            String n = "pak" + i;
            TextView c;
            if (data.pak[i]) c = ui.chip(n + " ✔", Color.rgb(126, 230, 154), Color.argb(36, 48, 209, 88));
            else if (i == 3) c = ui.chip(n + " (Expansion Pack)", Ui.SECONDARY, Color.argb(18, 255, 255, 255));
            else c = ui.chip(n + " missing", Color.rgb(255, 191, 92), Color.argb(36, 255, 159, 10));
            chips.addView(c, ui.gapLeft(Ui.wrap(), i == 0 ? 0 : 8));
        }
        if (data.extra > 0) {
            chips.addView(ui.chip("+" + data.extra + " more", Ui.SECONDARY, Color.argb(18, 255, 255, 255)), ui.gapLeft(Ui.wrap(), 8));
        }
        g.addView(chips);
        // the Expansion Pack, clearly either way
        if (data.pak[0]) {
            LinearLayout ex = ui.row();
            ex.setPadding(ui.dp(12), ui.dp(8), ui.dp(12), ui.dp(8));
            TextView t;
            if (data.expansion) {
                ex.setBackground(Ui.round(Color.argb(36, 48, 209, 88), ui.dp(10), 0, 0));
                t = ui.text("✦  Expansion Pack found · Virtual Voyager and its Holomatch maps", 17, Color.rgb(126, 230, 154));
            } else {
                ex.setBackground(Ui.round(Color.argb(18, 255, 255, 255), ui.dp(10), 0, 0));
                t = ui.text("✧  No Expansion Pack · add its files to play Virtual Voyager and its maps", 17, Color.rgb(209, 209, 214));
            }
            ex.addView(t, Ui.weight(1));
            g.addView(ex, ui.gapTop(Ui.matchWrap(), 10));
        }
        if (withButton) {
            LinearLayout br = ui.row();
            br.setGravity(Gravity.END);
            br.addView(ui.button(data.pak[0] ? "Add Game Files…" : "Choose…", !data.pak[0], new View.OnClickListener() {
                @Override public void onClick(View v) { showGameFiles(); }
            }));
            g.addView(br, ui.gapTop(Ui.matchWrap(), 10));
        }
    }

    // ---------------------------------------------------------------- saves & settings

    private Uri backupTree() {
        String s = backupPrefs.getString(SaveBackup.KEY_TREE, null);
        if (s == null) return null;
        Uri u = Uri.parse(s);
        for (UriPermission p : getContentResolver().getPersistedUriPermissions()) {
            if (p.getUri().equals(u) && p.isWritePermission()) return u;
        }
        return null;        // the permission is gone (e.g. a reinstall): pick the folder again
    }

    private void savesGroup(LinearLayout g) {
        Uri tree = backupTree();
        g.addView(ui.text("Uninstalling the app deletes its folder: the copied game files, your saves and your settings. "
                + "So your saves and settings (not the game files) are copied to a folder you pick, each time you come back "
                + "here from the game. After reinstalling, use Restore… and pick the same folder.", 15, Ui.SECONDARY));
        LinearLayout st;
        if (tree == null) {
            st = ui.status("⚠", Ui.ORANGE, "Not backed up yet: your saves would go with an uninstall.");
        } else if (backingUp) {
            st = ui.status("⏳", Ui.SECONDARY, "Backing up to " + SaveBackup.displayName(tree) + "…");
        } else {
            long t = backupPrefs.getLong(SaveBackup.KEY_TIME, 0);
            String result = backupPrefs.getString(SaveBackup.KEY_RESULT, "");
            boolean ok = result.isEmpty();
            long now = System.currentTimeMillis();
            String when = t == 0 ? "not yet" : now - t < DateUtils.MINUTE_IN_MILLIS ? "just now"
                    : DateUtils.getRelativeTimeSpanString(t, now, DateUtils.MINUTE_IN_MILLIS).toString();
            st = ui.status(ok ? "✔" : "⚠", ok ? Ui.GREEN : Ui.ORANGE,
                    "Backed up to " + SaveBackup.displayName(tree) + " · " + when + (ok ? "" : " · " + result));
        }
        g.addView(st, ui.gapTop(Ui.matchWrap(), 6));
        LinearLayout br = ui.row();
        br.setGravity(Gravity.END);
        if (tree != null) {
            br.addView(ui.smallButton("Other Folder…", new View.OnClickListener() {
                @Override public void onClick(View v) { pickTree(PICK_BACKUP); }
            }));
            br.addView(new View(this), new LinearLayout.LayoutParams(ui.dp(10), 1));
        }
        br.addView(ui.smallButton(tree == null ? "Choose Folder…" : "Back Up Now", new View.OnClickListener() {
            @Override public void onClick(View v) {
                if (backupTree() == null) pickTree(PICK_BACKUP);
                else runBackup();
            }
        }));
        br.addView(ui.smallButton("Restore…", new View.OnClickListener() {
            @Override public void onClick(View v) {
                dialog("Restore saves and settings?",
                        "Pick the folder your saves were backed up to (for example Documents/AwayTeamEF). Its saves and settings "
                                + "replace the ones here with the same names; game files are never touched.",
                        new String[]{"Cancel", "Pick Folder…"}, 1, new Choice() {
                            @Override public void chose(int i) { if (i == 1) pickTree(PICK_RESTORE); }
                        });
            }
        }), ui.gapLeft(Ui.wrap(), 10));
        g.addView(br, ui.gapTop(Ui.matchWrap(), 10));
    }

    private void pickTree(int request) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        // start in Documents (the suggested place: Documents/AwayTeamEF) or Download (game files)
        i.putExtra(DocumentsContract.EXTRA_INITIAL_URI, DocumentsContract.buildDocumentUri(
                "com.android.externalstorage.documents", request == PICK_FOLDER ? "primary:Download" : "primary:Documents"));
        startPicker(i, request);
    }

    private void startPicker(Intent i, int request) {
        try {
            startActivityForResult(i, request);
        } catch (RuntimeException e) {
            Log.e(TAG, "no file picker", e);
            dialog("The file picker didn't open", "This headset didn't open Android's file picker (" + e.getMessage() + ").",
                    new String[]{"OK"}, 0, null);
        }
    }

    /** After the game (and on opening): back up when a folder is set and the last run is over 30 s old. */
    private void autoBackup(boolean fromGame) {
        // never while restoring: the picker's return resumes this activity before the restore has
        // copied anything back, and a backup then would overwrite the backup's newer files
        if (backupTree() == null || backingUp || restoring) return;
        if (!fromGame && System.currentTimeMillis() - lastBackupStart < 30000) return;
        runBackup();
    }

    private void runBackup() {
        final Uri tree = backupTree();
        if (tree == null || backingUp || restoring) return;
        backingUp = true;
        lastBackupStart = System.currentTimeMillis();
        if (page.equals("main")) showMain();
        new Thread(new Runnable() {
            @Override public void run() {
                final SaveBackup.Result r = SaveBackup.backup(StartActivity.this, tree, obb);
                handler.post(new Runnable() {
                    @Override public void run() {
                        backingUp = false;
                        backupPrefs.edit().putLong(SaveBackup.KEY_TIME, System.currentTimeMillis())
                                .putString(SaveBackup.KEY_RESULT, r.error != null ? "failed: " + r.error
                                        : r.failed > 0 ? r.failed + " files couldn't be copied" : "").apply();
                        if (!isFinishing() && page.equals("main")) showMain();
                    }
                });
            }
        }, "backup").start();
    }

    private void restore(final Uri tree) {
        restoring = true;
        progressDialog("Restoring saves and settings", "Copying from " + SaveBackup.displayName(tree) + " …", null);
        new Thread(new Runnable() {
            @Override public void run() {
                final SaveBackup.Result r = SaveBackup.restore(StartActivity.this, tree, obb);
                handler.post(new Runnable() {
                    @Override public void run() {
                        importing = false;
                        restoring = false;
                        lastBackupStart = System.currentTimeMillis();   // what's here now is the backup's
                        hideDialog();
                        showMain();
                        if (r.error != null || r.failed > 0) {
                            dialog("Restoring failed", r.error != null ? r.error : r.failed + " files couldn't be copied back.",
                                    new String[]{"OK"}, 0, null);
                        } else {
                            dialog("Saves and settings restored", r.copied + " files copied back. New backups go to the same folder.",
                                    new String[]{"OK"}, 0, null);
                        }
                    }
                });
            }
        }, "restore").start();
    }

    /** A backup folder holds baseEF (any case); anything else is probably the wrong folder. */
    private boolean looksLikeBackup(Uri tree) {
        try (android.database.Cursor c = getContentResolver().query(
                DocumentsContract.buildChildDocumentsUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree)),
                new String[]{DocumentsContract.Document.COLUMN_DISPLAY_NAME}, null, null, null)) {
            while (c != null && c.moveToNext()) {
                if ("baseef".equalsIgnoreCase(c.getString(0))) return true;
            }
        } catch (Exception e) {
            Log.w(TAG, "restore: can't list " + tree, e);
        }
        return false;
    }

    // ---------------------------------------------------------------- Game Files page (import)

    private void showGameFiles() {
        Data data = data();
        LinearLayout p = ui.column();
        p.addView(topBar("Game Files"));
        TextView intro = ui.text("Choose an image of the game's CD or of the Expansion Pack's CD, a BaseEF folder from an installed game, "
                + "or GOG's installer. The game files are copied into the app's own folder (about 600 MB for a standard install), "
                + "so the original is no longer needed afterwards.", 18, Color.rgb(199, 199, 204));
        intro.setPadding(0, ui.dp(12), 0, ui.dp(14));
        p.addView(intro);

        LinearLayout tiles = ui.row();
        tiles.setGravity(Gravity.TOP);
        tile(tiles, "💿", "CD image", ".iso, or .bin with its .cue (pick both). The game's CD has pak0; the Expansion Pack's CD brings "
                + "patch 1.2 (pak1, pak2) and pak3.", "Choose Image…", true, new Runnable() {
            @Override public void run() { pickFiles(PICK_IMAGE); }
        });
        tile(tiles, "📁", "BaseEF folder", "The BaseEF folder of an installed Elite Force (GOG, Steam, or a PC with patch 1.2), copied onto "
                + "the Quest. Its .pk3 files are copied.", "Choose Folder…", true, new Runnable() {
            @Override public void run() { pickTree(PICK_FOLDER); }
        });
        boolean gog = LauncherCore.LOADED && LauncherCore.haveGog();
        tile(tiles, "📦", "GOG installer", gog ? "GOG's Windows installer (setup_….exe). Its .pk3 files are unpacked; nothing is installed."
                : "This build can't unpack GOG's installer.", "Choose Installer…", gog, new Runnable() {
            @Override public void run() { pickFiles(PICK_INSTALLER); }
        });
        p.addView(tiles);

        LinearLayout help = ui.group(p, "Getting the file onto the Quest");
        help.addView(ui.text("Connect the Quest to a computer with a USB cable, allow access to its files in the headset, and copy the "
                + "file or folder into the Quest's Download folder. Or download GOG's installer with the Quest's browser. "
                + "After importing you can delete what you put in Download.", 17, Ui.SECONDARY));

        LinearLayout now = ui.group(p, "Found now");
        gameFilesStatus(now, data, false);
        setPage("gamefiles", p);
    }

    private void tile(LinearLayout parent, String icon, String title, String text, String button, boolean enabled, final Runnable r) {
        LinearLayout t = ui.column();
        t.setBackground(Ui.round(Ui.FILL, ui.dp(18), ui.dp(1.5f), Ui.STROKE));
        t.setPadding(ui.dp(20), ui.dp(18), ui.dp(20), ui.dp(18));
        t.addView(ui.text(icon, 34, Ui.TEXT));
        t.addView(ui.bold(title, 23, Ui.TEXT), ui.gapTop(Ui.wrap(), 6));
        TextView tx = ui.text(text, 16, Ui.SECONDARY);
        tx.setMinLines(4);
        t.addView(tx, ui.gapTop(Ui.matchWrap(), 6));
        TextView b = ui.button(button, true, new View.OnClickListener() {
            @Override public void onClick(View v) { r.run(); }
        });
        b.setEnabled(enabled);
        t.addView(b, ui.gapTop(Ui.matchWrap(), 12));
        LinearLayout.LayoutParams lp = Ui.weight(1);
        if (parent.getChildCount() > 0) lp.leftMargin = ui.dp(16);
        lp.bottomMargin = ui.dp(16);
        parent.addView(t, lp);
    }

    private void pickFiles(int request) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, request == PICK_IMAGE);     // a .bin and its .cue
        i.putExtra(DocumentsContract.EXTRA_INITIAL_URI,
                DocumentsContract.buildDocumentUri("com.android.externalstorage.documents", "primary:Download"));
        startPicker(i, request);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent intent) {
        super.onActivityResult(request, result, intent);
        if (result != RESULT_OK || intent == null) {
            Log.i(TAG, "picker " + request + " cancelled");
            return;
        }
        List<Uri> uris = new ArrayList<Uri>();
        ClipData clip = intent.getClipData();
        if (clip != null) {
            for (int i = 0; i < clip.getItemCount(); i++) uris.add(clip.getItemAt(i).getUri());
        } else if (intent.getData() != null) {
            uris.add(intent.getData());
        }
        if (uris.isEmpty()) return;
        Uri first = uris.get(0);
        Log.i(TAG, "picker " + request + ": " + uris);
        switch (request) {
            case PICK_BACKUP:
            case PICK_RESTORE:
                if (request == PICK_RESTORE && !looksLikeBackup(first)) {
                    dialog("That folder has no backup", "“" + SaveBackup.displayName(first) + "” has no baseEF folder, so "
                            + "it isn't a backup of AwayTeamEF's saves. Pick the folder you chose for the backup.",
                            new String[]{"OK"}, 0, null);
                    return;
                }
                try {
                    getContentResolver().takePersistableUriPermission(first,
                            Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                } catch (RuntimeException e) {
                    Log.w(TAG, "no persistable permission for " + first, e);
                }
                backupPrefs.edit().putString(SaveBackup.KEY_TREE, first.toString()).apply();
                if (request == PICK_RESTORE) restore(first);
                else runBackup();
                break;
            case PICK_FOLDER:
                try {
                    startImport(GameImport.folder(this, first), false);
                } catch (Exception e) {
                    importFailed(e);
                }
                break;
            default:
                try {
                    startImport(GameImport.files(this, uris), true);
                } catch (Exception e) {
                    importFailed(e);
                }
        }
    }

    private void importFailed(Exception e) {
        Log.e(TAG, "import: can't open the picked files", e);
        dialog("The picked files can't be read", String.valueOf(e.getMessage()), new String[]{"OK"}, 0, null);
    }

    /** Plans the copy with the core, checks the space, then runs it behind the progress panel. */
    private void startImport(final GameImport picked, boolean unpack) {
        if (!LauncherCore.LOADED) {
            picked.close();
            dialog("Importing isn't in this build", "liblauncher.so is missing.", new String[]{"OK"}, 0, null);
            return;
        }
        String[] alert = new String[2];
        long job;
        String path = unpack ? picked.unpackPath() : null;
        if (path != null && !path.toLowerCase(Locale.ROOT).endsWith(".pk3")) {
            // the chooser's rule: a disc image without pak0 (the Expansion Pack's) is refused while the
            // game has no pak0 yet; with pak0 present it adds what's new (Add Game Files)
            job = LauncherCore.planUnpack(path, data().pak[0], homeBase.getPath(), alert);
        } else {
            // a folder's .pk3s, or .pk3 files picked directly
            job = LauncherCore.planAdd(picked.paths.toArray(new String[0]), homeBase.getPath(), alert);
        }
        if (job == 0) {
            picked.close();
            dialog(alert[0] != null ? alert[0] : "Nothing to add", alert[1] != null ? alert[1] : "", new String[]{"OK"}, 0, null);
            return;
        }
        if (!LauncherCore.jobCheckSpace(job, alert)) {
            LauncherCore.jobFree(job);
            picked.close();
            dialog(alert[0], alert[1], new String[]{"OK"}, 0, null);
            return;
        }
        final long j = job;
        String[] texts = LauncherCore.jobTexts(j);
        final long total = Math.max(1, LauncherCore.jobTotal(j));
        final ProgressBar bar = progressDialog(texts[0], texts[1].replace(obb.getPath(), "Android/obb/" + getPackageName()), new Runnable() {
            @Override public void run() { LauncherCore.jobCancel(j); }
        });
        final TextView amount = (TextView) overlay.findViewWithTag("amount");
        final Runnable tick = new Runnable() {
            @Override public void run() {
                if (!importing) return;
                long done = LauncherCore.jobProgress(j);
                bar.setProgress((int) (done * 1000 / total));
                if (amount != null) amount.setText(mb(done) + " of " + mb(total));
                handler.postDelayed(this, 200);
            }
        };
        handler.post(tick);
        new Thread(new Runnable() {
            @Override public void run() {
                final String[] fail = new String[2];
                final int r = LauncherCore.jobRun(j, fail);
                handler.post(new Runnable() {
                    @Override public void run() {
                        importing = false;
                        LauncherCore.jobFree(j);
                        picked.close();
                        hideDialog();
                        Log.i(TAG, "import: " + (r == 1 ? "done" : r == 0 ? "failed: " + fail[1] : "cancelled"));
                        if (page.equals("gamefiles")) showGameFiles();
                        if (r == 0) {
                            dialog(fail[0], fail[1], new String[]{"OK"}, 0, null);
                        } else if (r == 1) {
                            Data d = data();
                            dialog("Game files added", d.canPlay() ? "The game is ready to play." + (d.expansion ? " The Expansion Pack is there too." : "")
                                    : !d.pak[0] ? "pak0.pk3 is still missing: add the game's CD image or a full BaseEF folder."
                                    : "Patch 1.2 (" + d.missingPatch() + ") is still missing: add the Expansion Pack's CD image or a patched BaseEF folder.",
                                    new String[]{"OK"}, 0, null);
                        }
                    }
                });
            }
        }, "import").start();
    }

    private static String mb(long bytes) {
        return String.format(Locale.ROOT, "%d MB", (bytes + 500000) / 1000000);
    }

    // ---------------------------------------------------------------- dialogs (drawn on the panel)

    interface Choice { void chose(int index); }

    private LinearLayout dialogBox() {
        overlay.removeAllViews();
        LinearLayout box = ui.column();
        box.setBackground(Ui.round(Color.rgb(42, 42, 45), ui.dp(22), ui.dp(1.5f), Color.rgb(72, 72, 74)));
        box.setPadding(ui.dp(28), ui.dp(24), ui.dp(28), ui.dp(22));
        box.setClickable(true);
        ScrollView sc = new ScrollView(this);
        sc.addView(box);
        overlay.addView(sc, new FrameLayout.LayoutParams(ui.dp(760), ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER));
        overlay.setVisibility(View.VISIBLE);
        return box;
    }

    private void dialog(String title, String text, String[] buttons, int primary, final Choice c) {
        LinearLayout box = dialogBox();
        box.addView(ui.bold(title, 24, Ui.TEXT));
        if (text != null && !text.isEmpty()) {
            TextView t = ui.text(text, 18, Color.rgb(199, 199, 204));
            t.setPadding(0, ui.dp(10), 0, 0);
            box.addView(t);
        }
        LinearLayout br = ui.row();
        br.setGravity(Gravity.END);
        for (int i = 0; i < buttons.length; i++) {
            final int k = i;
            br.addView(ui.button(buttons[i], i == primary, new View.OnClickListener() {
                @Override public void onClick(View v) {
                    hideDialog();
                    if (c != null) c.chose(k);
                }
            }), ui.gapLeft(Ui.wrap(), i == 0 ? 0 : 12));
        }
        box.addView(br, ui.gapTop(Ui.matchWrap(), 18));
    }

    private ProgressBar progressDialog(String title, String label, final Runnable cancel) {
        importing = true;
        LinearLayout box = dialogBox();
        box.addView(ui.bold(title, 24, Ui.TEXT));
        TextView l = ui.text(label, 17, Color.rgb(199, 199, 204));
        l.setPadding(0, ui.dp(8), 0, 0);
        box.addView(l);
        ProgressBar bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        bar.setIndeterminate(cancel == null);
        box.addView(bar, ui.gapTop(new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ui.dp(24)), 14));
        LinearLayout r = ui.row();
        TextView amount = ui.text("", 17, Ui.SECONDARY);
        amount.setTag("amount");
        r.addView(amount, Ui.weight(1));
        if (cancel != null) {
            r.addView(ui.button("Cancel", false, new View.OnClickListener() {
                @Override public void onClick(View v) {
                    v.setEnabled(false);
                    cancel.run();
                }
            }));
        }
        box.addView(r, ui.gapTop(Ui.matchWrap(), 10));
        return bar;
    }

    private void hideDialog() {
        overlay.removeAllViews();
        overlay.setVisibility(View.GONE);
    }

    // ---------------------------------------------------------------- the player name

    /** The one name: Holomatch's (+set name) and co-op's (+set coop_name). */
    private String playerName() {
        String n = prefs.getString(PREF_NAME, null);
        if (n == null || n.isEmpty()) {
            // as the desktop launchers: hmconfig.cfg's name unless it's absent or UnnamedPlayer, else
            // the default player name (the user's first name; a Quest has none, so "Crewman")
            n = hmconfigName();
            if (n == null || n.isEmpty() || n.equals("UnnamedPlayer")) n = "Crewman";
        }
        return n;
    }

    private String coopName() {
        return LauncherCore.LOADED ? LauncherCore.coopSafeName(playerName()) : playerName();
    }

    private String hmconfigName() {
        File f = new File(homeBase, "hmconfig.cfg");
        if (!f.isFile()) return null;
        Pattern p = Pattern.compile("^\\s*seta?\\s+name\\s+\"([^\"]*)\"", Pattern.CASE_INSENSITIVE);
        String found = null;
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            for (String line; (line = r.readLine()) != null; ) {
                Matcher m = p.matcher(line);
                if (m.find()) found = m.group(1);
            }
        } catch (Exception e) {
            Log.w(TAG, "hmconfig.cfg: " + e);
        }
        return found;
    }

    private void editName() {
        setPage("name", TextEntry.page(ui, "Player Name",
                "Shown to other players in Holomatch and co-op. Letters, digits and most symbols; \" + ; \\ % are left out.",
                playerName(), TextEntry.Mode.NAME, MAX_NAME, new Runnable() {
                    @Override public void run() { showMain(); }
                }, new TextEntry.Done() {
                    @Override public void done(String text) {
                        String clean = LauncherCore.LOADED ? LauncherCore.cleanName(text) : text.trim();
                        prefs.edit().putString(PREF_NAME, clean).apply();
                        Log.i(TAG, "player name set");
                        showMain();
                    }
                }));
    }

    // ---------------------------------------------------------------- Holomatch: Browse Servers

    private void showBrowser() {
        if (browser != null) {
            setPage("browser", browser.root);
            return;
        }
        browser = new BrowserPage(ui, getSharedPreferences(BrowserPage.PREFS, MODE_PRIVATE), homeBase.getPath(), new BrowserPage.Host() {
            @Override public void back() { showMain(); }
            @Override public void join(String address, String name, int mapState, String map, boolean needPass) {
                joinServer(address, name, mapState, map, needPass);
            }
            @Override public void joinByAddress() {
                setPage("address", TextEntry.page(ui, "Join by Address", "A server's address, like 203.0.113.5:27960.", "",
                        TextEntry.Mode.ADDRESS, MAX_ADDRESS, new Runnable() {
                            @Override public void run() { showBrowser(); }
                        }, new TextEntry.Done() {
                            @Override public void done(String text) {
                                String a = text.trim();
                                if (!a.matches("[0-9.:]+")) {
                                    dialog("That isn't an address", "Type the server's address, like 203.0.113.5:27960.", new String[]{"OK"}, 0, null);
                                    return;
                                }
                                launchHolomatch(a, "");
                            }
                        }));
            }
        });
        setPage("browser", browser.root);
    }

    private void joinServer(final String address, final String name, int mapState, String map, final boolean needPass) {
        if (mapState == LauncherCore.MAP_EXPANSION) {
            dialog("This server's map is from the Expansion Pack.", "“" + name + "” is running " + map
                    + ", which isn't in your game files. Servers can't send the Expansion Pack's maps, so joining will fail. "
                    + "Add the Expansion Pack's files with Add Game Files… on the start screen, or pick another server.",
                    new String[]{"Cancel", "Join Anyway"}, 0, new Choice() {
                        @Override public void chose(int i) { if (i == 1) joinServer(address, name, LauncherCore.MAP_HAVE, "", needPass); }
                    });
            return;
        }
        if (needPass) {
            setPage("password", TextEntry.page(ui, "“" + Ui.ellipsize(name, 40) + "” needs a password.",
                    "Ask the person running the server for it.", "", TextEntry.Mode.PASSWORD, 63, new Runnable() {
                        @Override public void run() { showBrowser(); }
                    }, new TextEntry.Done() {
                        @Override public void done(String text) { launchHolomatch(address, text.replace("\"", "")); }
                    }));
            return;
        }
        launchHolomatch(address, "");
    }

    private void launchHolomatch(String address, String password) {
        StringBuilder a = new StringBuilder("+set com_holomatch 1 +set name ").append(q(playerName()));
        if (!password.isEmpty()) a.append(" +set password ").append(q(password));
        a.append(" +connect ").append(address);
        launch(a.toString());
    }

    // ---------------------------------------------------------------- Co-op: Join

    private void showCoopJoin() {
        LinearLayout p = ui.column();
        p.addView(topBar("Join a Co-op Game"));
        LinearLayout cols = ui.row();
        cols.setGravity(Gravity.TOP);
        LinearLayout left = ui.column(), right = ui.column();
        cols.addView(left, Ui.weight(1));
        cols.addView(right, ui.gapLeft(Ui.weight(1), 26));
        p.addView(cols, ui.gapTop(Ui.matchWrap(), 16));

        // left: the LAN search (CoopLan, the desktop launchers' query)
        searchButton = ui.button("Search Local Network", false, new View.OnClickListener() {
            @Override public void onClick(View v) { search(); }
        });
        left.addView(searchButton, Ui.matchWrap());
        searchStatus = ui.text("Finds co-op games on this Wi-Fi whose host shows the game on the network.", 17, Ui.SECONDARY);
        searchStatus.setPadding(0, ui.dp(8), 0, ui.dp(4));
        left.addView(searchStatus);
        results = ui.column();
        left.addView(results, Ui.matchWrap());

        // player 2: Munro's look and the character (the desktop's Co-op sheet: coop_sex, coop_look)
        TextView gap = new TextView(this);
        left.addView(gap, new LinearLayout.LayoutParams(1, ui.dp(12)));
        LinearLayout p2 = ui.group(left, "Player 2");
        LinearLayout sexRow = ui.row();
        sexRow.addView(ui.text("Munro:", 19, Color.rgb(209, 209, 214)), new LinearLayout.LayoutParams(ui.dp(120), ViewGroup.LayoutParams.WRAP_CONTENT));
        final String sex = prefs.getString(PREF_SEX, "male");
        String[] sexes = {"male", "female"}, labels = {"Male", "Female"};
        for (int i = 0; i < 2; i++) {
            final String s = sexes[i];
            TextView b = ui.button(labels[i], s.equals(sex), new View.OnClickListener() {
                @Override public void onClick(View v) {
                    prefs.edit().putString(PREF_SEX, s).apply();
                    showCoopJoin();
                }
            });
            sexRow.addView(b, ui.gapLeft(Ui.weight(1), i == 0 ? 0 : 10));
        }
        p2.addView(sexRow);
        LinearLayout lookRow = ui.row();
        lookRow.addView(ui.text("Character:", 19, Color.rgb(209, 209, 214)), new LinearLayout.LayoutParams(ui.dp(120), ViewGroup.LayoutParams.WRAP_CONTENT));
        lookRow.addView(ui.button(characterName(prefs.getString(PREF_LOOK, "")) + "  ›", false, new View.OnClickListener() {
            @Override public void onClick(View v) { showCharacters(); }
        }), Ui.weight(1));
        p2.addView(lookRow, ui.gapTop(Ui.matchWrap(), 10));
        TextView note = ui.text("How player 2 looks on both screens, with the voice of the Munro above. The story still speaks to the host's Munro.", 15, Ui.SECONDARY);
        note.setPadding(0, ui.dp(8), 0, 0);
        p2.addView(note);

        // right: the address, the number pad, the password, Join
        LinearLayout nameLine = ui.row();
        nameLine.addView(ui.text("Host's address", 17, Ui.SECONDARY), Ui.weight(1));
        TextView as = ui.text("", 17, Ui.SECONDARY);
        android.text.SpannableStringBuilder sb = new android.text.SpannableStringBuilder("Joining as ");
        sb.append(Ui.colourText(coopName()));
        as.setText(sb);
        nameLine.addView(as);
        right.addView(nameLine);
        addressView = ui.text("", 28, Ui.TEXT);
        addressView.setTypeface(android.graphics.Typeface.MONOSPACE);
        addressView.setGravity(Gravity.CENTER_VERTICAL);
        addressView.setMinHeight(ui.dp(68));
        addressView.setPadding(ui.dp(18), ui.dp(8), ui.dp(18), ui.dp(8));
        addressView.setBackground(Ui.round(Ui.FIELD, ui.dp(12), ui.dp(1.5f), Color.rgb(72, 72, 74)));
        right.addView(addressView, ui.gapTop(Ui.matchWrap(), 6));
        showAddress();
        padRow(right, new String[]{"1", "2", "3", "4", "5", "6"});
        padRow(right, new String[]{"7", "8", "9", "0", ".", ":"});
        LinearLayout fr = ui.row();
        padKey(fr, "⌫ Back", new View.OnClickListener() {
            @Override public void onClick(View v) {
                if (address.length() > 0) address.setLength(address.length() - 1);
                showAddress();
            }
        });
        padKey(fr, "Clear", new View.OnClickListener() {
            @Override public void onClick(View v) { address.setLength(0); showAddress(); }
        });
        right.addView(fr, ui.gapTop(Ui.matchWrap(), 8));
        LinearLayout pw = ui.row();
        pw.addView(ui.text("Password:", 19, Color.rgb(209, 209, 214)), new LinearLayout.LayoutParams(ui.dp(120), ViewGroup.LayoutParams.WRAP_CONTENT));
        pw.addView(ui.button(coopPassword.isEmpty() ? "None (if the host set one)" : "••••••  Change", false,
                new View.OnClickListener() {
                    @Override public void onClick(View v) { editCoopPassword(null); }
                }), Ui.weight(1));
        right.addView(pw, ui.gapTop(Ui.matchWrap(), 12));
        right.addView(ui.button("Join", true, new View.OnClickListener() {
            @Override public void onClick(View v) { coopJoin(address.toString().trim()); }
        }), ui.gapTop(Ui.matchWrap(), 12));
        setPage("coop", p);
    }

    private void editCoopPassword(final String thenJoin) {
        setPage("password", TextEntry.page(ui, thenJoin != null ? "This co-op game needs a password." : "Co-op Password",
                "The password the host set, if any. Ask the host for it.", coopPassword, TextEntry.Mode.PASSWORD, 63,
                new Runnable() {
                    @Override public void run() { showCoopJoin(); }
                }, new TextEntry.Done() {
                    @Override public void done(String text) {
                        coopPassword = text.replace("\"", "");
                        if (thenJoin != null && !coopPassword.isEmpty()) coopJoin(thenJoin);
                        else showCoopJoin();
                    }
                }));
    }

    private void padRow(LinearLayout parent, String[] keys) {
        LinearLayout r = ui.row();
        for (final String k : keys) {
            padKey(r, k, new View.OnClickListener() {
                @Override public void onClick(View v) {
                    if (address.length() < MAX_ADDRESS) address.append(k);
                    showAddress();
                }
            });
        }
        parent.addView(r, ui.gapTop(Ui.matchWrap(), 8));
    }

    private void padKey(LinearLayout row, String label, View.OnClickListener l) {
        TextView k = ui.text(label, 24, Ui.TEXT);
        k.setGravity(Gravity.CENTER);
        k.setMinHeight(ui.dp(64));
        k.setPadding(0, ui.dp(8), 0, ui.dp(8));
        k.setBackground(ui.buttonBackground(Color.rgb(47, 47, 51)));
        k.setClickable(true);
        k.setOnClickListener(l);
        LinearLayout.LayoutParams lp = Ui.weight(1);
        lp.leftMargin = lp.rightMargin = ui.dp(4);
        row.addView(k, lp);
    }

    private void showAddress() {
        if (addressView == null) return;
        if (address.length() == 0) {
            addressView.setTextColor(Ui.TERTIARY);
            addressView.setText("192.168.1.20");
        } else {
            addressView.setTextColor(Ui.TEXT);
            addressView.setText(address.toString());
        }
    }

    /** id -> {id, name, group}: the crew and the Hazard Team (efgame/src/game/coop_characters.h through the core) */
    private Map<String, String[]> characters() {
        Map<String, String[]> m = new java.util.LinkedHashMap<String, String[]>();
        if (!LauncherCore.LOADED) return m;
        for (String line : LauncherCore.coopCharacters()) {
            String[] f = line.split(LauncherCore.SEP, -1);
            if (f.length == 3 && (f[2].equals("crew") || f[2].equals("hazard"))) m.put(f[0], f);
        }
        return m;
    }

    private String characterName(String id) {
        String[] f = id.isEmpty() ? null : characters().get(id);
        return f == null ? "Munro (default)" : f[1];
    }

    private void showCharacters() {
        LinearLayout p = ui.column();
        LinearLayout top = ui.row();
        top.addView(ui.button("‹ Back", false, new View.OnClickListener() {
            @Override public void onClick(View v) { showCoopJoin(); }
        }));
        TextView t = ui.bold("Player 2's Character", 28, Ui.TEXT);
        t.setPadding(ui.dp(18), 0, 0, 0);
        top.addView(t, Ui.weight(1));
        p.addView(top);
        String current = prefs.getString(PREF_LOOK, "");
        characterButton(p, "", "Munro (default)", current);
        String group = "";
        LinearLayout grid = null;
        for (String[] f : characters().values()) {
            if (!f[2].equals(group)) {
                if (grid != null) while (grid.getChildCount() < 3) grid.addView(new View(this), ui.gapLeft(Ui.weight(1), 10));
                group = f[2];
                TextView h = ui.bold(group.equals("crew") ? "VOYAGER CREW" : "HAZARD TEAM", 15, Ui.SECONDARY);
                h.setPadding(ui.dp(2), ui.dp(16), 0, ui.dp(4));
                p.addView(h);
                grid = null;
            }
            if (grid == null || grid.getChildCount() == 3) {
                grid = ui.row();
                p.addView(grid, ui.gapTop(Ui.matchWrap(), 8));
            }
            characterButton(grid, f[0], f[1], current);
        }
        // keep the last row's buttons as wide as the others
        if (grid != null) while (grid.getChildCount() < 3) grid.addView(new View(this), ui.gapLeft(Ui.weight(1), 10));
        setPage("characters", p);
    }

    private void characterButton(LinearLayout parent, final String id, String name, String current) {
        boolean on = id.equals(current);
        TextView b = ui.button((on ? "✓ " : "") + name, on, new View.OnClickListener() {
            @Override public void onClick(View v) {
                prefs.edit().putString(PREF_LOOK, id).apply();
                showCoopJoin();
            }
        });
        if (parent.getOrientation() == LinearLayout.HORIZONTAL) {
            parent.addView(b, ui.gapLeft(Ui.weight(1), parent.getChildCount() == 0 ? 0 : 10));
        } else {
            parent.addView(b, ui.gapTop(Ui.matchWrap(), 14));
        }
    }

    /** Asks the LAN for co-op hosts on a worker thread (the UI thread only shows the answer). */
    private void search() {
        if (searching) return;
        searching = true;
        searchButton.setEnabled(false);
        results.removeAllViews();
        searchStatus.setText("Searching the local network…");
        Log.i(TAG, "co-op LAN search started");
        new Thread(new Runnable() {
            @Override public void run() {
                List<CoopLan.Game> games = null;
                String error = null;
                try {
                    games = CoopLan.scan(2.5, null);
                } catch (Throwable t) {
                    error = t.getMessage() == null ? t.toString() : t.getMessage();
                }
                final List<CoopLan.Game> found = games;
                final String err = error;
                runOnUiThread(new Runnable() {
                    @Override public void run() { showResults(found, err); }
                });
            }
        }, "coop-lan-search").start();
    }

    private void showResults(List<CoopLan.Game> games, String error) {
        searching = false;
        if (isFinishing() || isDestroyed() || !page.equals("coop")) return;
        searchButton.setEnabled(true);
        results.removeAllViews();
        if (games == null) {
            Log.w(TAG, "co-op LAN search failed: " + error);
            searchStatus.setText("The search could not run (" + error + "). Type the host's address instead.");
            return;
        }
        Log.i(TAG, "co-op LAN search found " + games.size());
        if (games.isEmpty()) {
            searchStatus.setText("No co-op games found. The host needs Co-op host running on the same Wi-Fi, "
                    + "with its game shown on the network; or type its address.");
            return;
        }
        searchStatus.setText(games.size() == 1 ? "1 co-op game found. Tap it to join." : games.size() + " co-op games found. Tap one to join.");
        for (final CoopLan.Game g : games) {
            final String problem = g.problem();
            TextView b = ui.text("", 18, problem.isEmpty() ? Ui.TEXT : Ui.SECONDARY);
            b.setText(g.title() + "\n" + (problem.isEmpty() ? g.detail() + (g.needsPassword() ? " · needs a password" : "")
                    : problem + " · " + g.address));
            b.setPadding(ui.dp(16), ui.dp(12), ui.dp(16), ui.dp(12));
            b.setMinHeight(ui.dp(60));
            b.setBackground(ui.buttonBackground(problem.isEmpty() ? Color.rgb(30, 56, 40) : Color.rgb(44, 40, 44)));
            b.setClickable(true);
            b.setOnClickListener(new View.OnClickListener() {
                @Override public void onClick(View v) {
                    address.setLength(0);
                    address.append(g.address);
                    showAddress();
                    if (!problem.isEmpty()) {
                        dialog(g.title(), problem, new String[]{"OK"}, 0, null);
                    } else if (g.needsPassword() && coopPassword.isEmpty()) {
                        editCoopPassword(g.address);
                    } else {
                        coopJoin(g.address);
                    }
                }
            });
            results.addView(b, ui.gapTop(Ui.matchWrap(), 8));
        }
    }

    private void coopJoin(String addr) {
        // the address goes on the engine's command line: digits, dots and a colon only (as the
        // desktop launchers accept)
        if (addr.isEmpty() || !addr.matches("[0-9.:]+")) {
            dialog("Type the host's address first", "Numbers and dots, e.g. 192.168.1.20 (or 192.168.1.20:29760).",
                    new String[]{"OK"}, 0, null);
            return;
        }
        prefs.edit().putString(PREF_ADDRESS, addr).apply();
        String sex = prefs.getString(PREF_SEX, "male").equals("female") ? "female" : "male";
        String look = prefs.getString(PREF_LOOK, "");
        StringBuilder a = new StringBuilder("+set com_holomatch 0 +set coop_connect ").append(q(addr))
                .append(" +set coop_sex ").append(sex);
        if (!look.isEmpty() && characters().containsKey(look)) a.append(" +set coop_look ").append(look);
        a.append(" +set coop_name ").append(q(coopName()));
        // the game is a process of its own here, so the password goes in its arguments (the
        // coop_password cvar, CVAR_TEMP: never written to a config); masked in the log line
        String logged = a + (coopPassword.isEmpty() ? "" : " +set coop_password \"***\"");
        if (!coopPassword.isEmpty()) a.append(" +set coop_password ").append(q(coopPassword));
        launch(a.toString(), logged);
    }

    // ---------------------------------------------------------------- launching

    /** A quoted engine argument ("" removed: the engine's parser has no escapes). */
    private static String q(String s) {
        return "\"" + s.replace("\"", "") + "\"";
    }

    private String version() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "";
        }
    }

    private void launch(String engineArgs) {
        launch(engineArgs, engineArgs.replaceAll("(\\+set password )\"[^\"]*\"", "$1\"***\""));
    }

    private void launch(String engineArgs, String logged) {
        Log.i(TAG, "StartActivity launching: " + logged);
        Intent i = new Intent(this, QuestActivity.class);
        i.putExtra("args", engineArgs);
        startActivity(i);
        finish();
    }
}
