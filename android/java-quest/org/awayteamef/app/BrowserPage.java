package org.awayteamef.app;

import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;
import java.util.TreeSet;

/**
 * Holomatch: Browse Servers. The desktop launchers' server browser (docs/launcher-spec/servers.md §8)
 * on the panel, driven by the same core (LauncherCore.browser*): source tabs Local / Combined /
 * Internet 1..5 (the default master servers), refresh, the status line, "Only favorites" and "Only
 * servers with people", the same columns, colours and sorting, favourites (stored like the desktop's
 * BrowserFavorites). A tap on a server joins it (StartActivity.joinServer): the Expansion Pack
 * warning and the password prompt come first when needed. Left out on the panel: the players
 * table, Map Info and the 3D preview.
 */
final class BrowserPage {
    private static final String TAG = "AwayTeamEF";
    static final String PREFS = "browser";

    interface Host {
        void back();
        void join(String address, String name, int mapState, String map, boolean needPass);
        void joinByAddress();
    }

    private final Ui ui;
    private final Host host;
    private final SharedPreferences prefs;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private long b;
    private int source;
    private int lastChanges = -1;
    private boolean lastBusy;
    private final TreeSet<String> favorites = new TreeSet<String>();
    final LinearLayout root;
    private final LinearLayout tabs, list;
    private final TextView status, onlyFav, onlyPeople;
    private final List<TextView> headers = new ArrayList<TextView>();
    private boolean closed;

    // columns: weight (0 = fixed dp in WIDTH), title, lc_column
    private static final String[] TITLES = {"☆", "Server", "🔒", "Map", "Players", "Humans", "Game", "cMod", "Ping"};
    private static final float[] WEIGHT = {0, 3.2f, 0, 1.7f, 0, 0, 1.7f, 0, 0};
    private static final int[] WIDTH = {48, 0, 34, 0, 88, 86, 0, 62, 62};
    private static final boolean[] RIGHT = {false, false, false, false, true, true, false, false, true};

    BrowserPage(Ui ui, SharedPreferences prefs, String homeBase, Host host) {
        this.ui = ui;
        this.host = host;
        this.prefs = prefs;
        String favs = prefs.getString("favorites", "");
        for (String f : favs.split("\n")) if (!f.isEmpty()) favorites.add(f);

        root = ui.column();
        LinearLayout top = ui.row();
        top.addView(ui.button("‹ Back", false, new View.OnClickListener() {
            @Override public void onClick(View v) { host.back(); }
        }));
        HorizontalScrollView hs = new HorizontalScrollView(ui.a);
        hs.setHorizontalScrollBarEnabled(false);
        tabs = ui.row();
        tabs.setBackground(Ui.round(Color.rgb(44, 44, 46), ui.dp(14), 0, 0));
        tabs.setPadding(ui.dp(4), ui.dp(4), ui.dp(4), ui.dp(4));
        hs.addView(tabs);
        top.addView(hs, ui.gapLeft(Ui.weight(1), 14));
        TextView refresh = ui.button("↻ Refresh", false, new View.OnClickListener() {
            @Override public void onClick(View v) { fetch(); }
        });
        top.addView(refresh, ui.gapLeft(Ui.wrap(), 10));
        root.addView(top);

        LinearLayout st = ui.row();
        status = ui.text("", 17, Ui.SECONDARY);
        st.addView(status, Ui.weight(1));
        onlyFav = toggle("Only favorites", "onlyFav");
        onlyPeople = toggle("Only servers with people", "onlyPeople");
        st.addView(onlyFav, ui.gapLeft(Ui.wrap(), 10));
        st.addView(onlyPeople, ui.gapLeft(Ui.wrap(), 10));
        root.addView(st, ui.gapTop(Ui.matchWrap(), 10));

        LinearLayout table = ui.column();
        table.setBackground(Ui.round(Color.argb(46, 0, 0, 0), ui.dp(16), ui.dp(1.5f), Ui.STROKE));
        LinearLayout head = ui.row();
        head.setBackground(Ui.round(Color.rgb(22, 22, 24), ui.dp(16), 0, 0));
        head.setPadding(ui.dp(6), ui.dp(4), ui.dp(6), ui.dp(4));
        for (int c = 0; c < TITLES.length; c++) {
            final int col = c;
            TextView h = cell(TITLES[c], c, 16, Ui.SECONDARY);
            h.setTypeface(Typeface.DEFAULT_BOLD);
            h.setMinHeight(ui.dp(48));
            h.setBackground(ui.buttonBackground(Color.rgb(22, 22, 24)));
            h.setClickable(true);
            h.setOnClickListener(new View.OnClickListener() {
                @Override public void onClick(View v) {
                    if (b != 0) LauncherCore.browserSortClick(b, col);
                    show();
                }
            });
            head.addView(h, params(c));
            headers.add(h);
        }
        table.addView(head);
        list = ui.column();
        table.addView(list);
        root.addView(table, ui.gapTop(Ui.matchWrap(), 10));

        LinearLayout foot = ui.row();
        foot.addView(ui.text("Tap a server to join it. Tap its star to keep it at the top.", 17, Ui.SECONDARY), Ui.weight(1));
        foot.addView(ui.smallButton("Join by Address…", new View.OnClickListener() {
            @Override public void onClick(View v) { host.joinByAddress(); }
        }));
        root.addView(foot, ui.gapTop(Ui.matchWrap(), 12));

        if (!LauncherCore.LOADED) {
            status.setText("The server browser isn't in this build (liblauncher.so is missing).");
            return;
        }
        b = LauncherCore.browserNew(homeBase);
        String[] labels = LauncherCore.browserSources(b);
        // 8.4: opens on Combined (Local when there are no masters)
        source = labels.length > 1 ? 1 : 0;
        for (int i = 0; i < labels.length; i++) {
            final int s = i;
            TextView t = ui.text(labels[i], 19, Color.rgb(209, 209, 214));
            t.setGravity(Gravity.CENTER);
            t.setMinHeight(ui.dp(54));
            t.setPadding(ui.dp(18), ui.dp(8), ui.dp(18), ui.dp(8));
            t.setClickable(true);
            t.setOnClickListener(new View.OnClickListener() {
                @Override public void onClick(View v) { select(s); }
            });
            tabs.addView(t);
        }
        select(source);
        handler.post(poll);
    }

    private TextView toggle(final String label, final String key) {
        final TextView t = ui.smallButton("", null);
        t.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                prefs.edit().putBoolean(key, !prefs.getBoolean(key, false)).apply();
                paintToggle(t, label, key);
                show();
            }
        });
        paintToggle(t, label, key);
        return t;
    }

    private void paintToggle(TextView t, String label, String key) {
        boolean on = prefs.getBoolean(key, false);
        t.setText((on ? "☑ " : "☐ ") + label);
        t.setTextColor(on ? Color.WHITE : Color.rgb(209, 209, 214));
        t.setBackground(ui.buttonBackground(on ? Ui.blend(Ui.ACCENT, Ui.BG, 0.45f) : Color.rgb(44, 44, 46)));
    }

    private void select(int s) {
        source = s;
        for (int i = 0; i < tabs.getChildCount(); i++) {
            TextView t = (TextView) tabs.getChildAt(i);
            boolean on = i == s;
            t.setTypeface(on ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT);
            t.setTextColor(on ? Color.WHITE : Color.rgb(209, 209, 214));
            t.setBackground(ui.buttonBackground(on ? Color.rgb(90, 90, 94) : Color.rgb(44, 44, 46)));
        }
        // 8.4: a source is fetched the first time it is shown; Refresh asks again
        if (!LauncherCore.browserFetched(b, source)) fetch();
        else show();
    }

    private void fetch() {
        if (b == 0) return;
        LauncherCore.browserFetch(b, source);
        show();
    }

    // reloads are coalesced (8.4): the UI looks at the change counter a few times a second
    private final Runnable poll = new Runnable() {
        @Override public void run() {
            if (closed || b == 0) return;
            int ch = LauncherCore.browserChanges(b);
            boolean busy = LauncherCore.browserBusy(b, source);
            if (ch != lastChanges || busy != lastBusy) show();
            handler.postDelayed(this, 400);
        }
    };

    private LinearLayout.LayoutParams params(int c) {
        LinearLayout.LayoutParams lp = WEIGHT[c] > 0 ? Ui.weight(WEIGHT[c])
                : new LinearLayout.LayoutParams(ui.dp(WIDTH[c]), LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.gravity = Gravity.CENTER_VERTICAL;
        return lp;
    }

    private TextView cell(CharSequence s, int c, float sp, int colour) {
        TextView t = ui.text("", sp, colour);
        t.setText(s);
        t.setSingleLine(true);
        t.setEllipsize(android.text.TextUtils.TruncateAt.END);
        t.setPadding(ui.dp(6), ui.dp(4), ui.dp(6), ui.dp(4));
        t.setGravity(Gravity.CENTER_VERTICAL | (RIGHT[c] ? Gravity.END : (c == 0 || c == 2 || c == 7 ? Gravity.CENTER_HORIZONTAL : Gravity.START)));
        return t;
    }

    void show() {
        if (b == 0 || closed) return;
        lastChanges = LauncherCore.browserChanges(b);
        lastBusy = LauncherCore.browserBusy(b, source);
        String s = LauncherCore.browserStatus(b, source);
        status.setText((lastBusy ? "⏳ " : "") + s);
        int key = LauncherCore.browserSortKey(b);
        for (int c = 0; c < headers.size(); c++) {
            String t = TITLES[c];
            if (key >= 0 && key / 2 == c) t += (key % 2 == 1) ? " ▲" : " ▼";
            headers.get(c).setText(t);
        }
        String[] rows = LauncherCore.browserRows(b, source, prefs.getBoolean("onlyFav", false),
                prefs.getBoolean("onlyPeople", false), favorites.toArray(new String[0]));
        list.removeAllViews();
        if (rows.length == 0) {
            TextView none = ui.text(lastBusy ? "Asking…" : "No servers to show.", 18, Ui.SECONDARY);
            none.setPadding(ui.dp(16), ui.dp(16), ui.dp(16), ui.dp(16));
            list.addView(none);
        }
        for (String line : rows) list.addView(row(line.split(LauncherCore.SEP, -1)));
    }

    private View row(final String[] f) {
        if (f.length < 15) return new View(ui.a);
        LinearLayout r = ui.row();
        r.setPadding(ui.dp(6), 0, ui.dp(6), 0);
        r.setMinimumHeight(ui.dp(58));
        r.setBackground(ui.buttonBackground(Color.argb(0, 0, 0, 0)));
        r.setClickable(true);
        final boolean fav = f[LauncherCore.R_FAVORITE].equals("1");
        final int mapState = parse(f[LauncherCore.R_MAPSTATE], 0);
        final boolean needPass = f[LauncherCore.R_NEEDPASS].equals("1");
        final String address = f[LauncherCore.R_ADDRESS];
        final String name = f[LauncherCore.R_NAME];
        r.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                Log.i(TAG, "Browser: join " + address);
                host.join(address, name, mapState, f[LauncherCore.R_MAP], needPass);
            }
        });

        TextView star = cell(fav ? "★" : "☆", 0, 24, fav ? Ui.AMBER : Ui.TERTIARY);
        star.setMinHeight(ui.dp(56));
        star.setClickable(true);
        star.setBackground(ui.buttonBackground(Color.argb(0, 0, 0, 0)));
        star.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                if (fav) favorites.remove(address); else favorites.add(address);
                StringBuilder sb = new StringBuilder();
                for (String a : favorites) sb.append(a).append('\n');      // sorted, as the desktop stores them
                prefs.edit().putString("favorites", sb.toString()).apply();
                show();
            }
        });
        r.addView(star, params(0));

        CharSequence n = Ui.colourText(name, f[LauncherCore.R_COLOURS]);
        if (f[LauncherCore.R_THISCOMPUTER].equals("1")) {
            android.text.SpannableStringBuilder sb = new android.text.SpannableStringBuilder(n);
            sb.append(" (this Quest)");
            n = sb;
        }
        r.addView(cell(n, 1, 19, Ui.TEXT), params(1));
        r.addView(cell(needPass ? "🔒" : "", 2, 16, Ui.SECONDARY), params(2));
        int mapColour = mapState == LauncherCore.MAP_EXPANSION ? Ui.PURPLE : mapState == LauncherCore.MAP_MISSING ? Ui.AMBER : Ui.TEXT;
        r.addView(cell(f[LauncherCore.R_MAP], 3, 19, mapColour), params(3));
        int players = parse(f[LauncherCore.R_PLAYERS], 0), max = parse(f[LauncherCore.R_MAX], 0);
        r.addView(cell(max > 0 ? players + "/" + max : String.valueOf(players), 4, 19, players == 0 ? Ui.SECONDARY : Ui.TEXT), params(4));
        int humans = parse(f[LauncherCore.R_HUMANS], -1);
        r.addView(cell(humans < 0 ? "?" : String.valueOf(humans), 5, 19, humans <= 0 ? Ui.SECONDARY : Ui.TEXT), params(5));
        r.addView(cell(f[LauncherCore.R_GAME], 6, 19, Ui.TEXT), params(6));
        r.addView(cell(f[LauncherCore.R_CMOD], 7, 18, Ui.TEXT), params(7));
        int ping = parse(f[LauncherCore.R_PING], 0);
        r.addView(cell(String.valueOf(ping), 8, 19, ping < 80 ? Ui.GREEN : ping < 160 ? Ui.TEXT : Ui.ORANGE), params(8));
        return r;
    }

    static int parse(String s, int d) {
        try { return Integer.parseInt(s.trim()); } catch (RuntimeException e) { return d; }
    }

    void close() {
        closed = true;
        handler.removeCallbacks(poll);
        if (b != 0) {
            final long old = b;
            b = 0;
            // lc_browser_free waits for the fetch workers: not on the UI thread
            new Thread(new Runnable() {
                @Override public void run() { LauncherCore.browserFree(old); }
            }, "browser-free").start();
        }
    }
}
