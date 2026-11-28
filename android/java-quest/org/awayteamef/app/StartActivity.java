package org.awayteamef.app;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.ScrollView;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.util.List;

/**
 * The Quest build's start screen (AndroidManifest-quest.xml; only compiled by make-apk.sh VR=1).
 *
 * A plain Android activity, shown as a flat panel in the headset before the game starts: buttons
 * that launch QuestActivity (the VR game activity) with the engine arguments that mode needs, the
 * same ones the desktop launchers put on the command line (efcode/launcher/lc_launch.c):
 *
 *   Single player   +set com_holomatch 0
 *   Co-op host      +set com_holomatch 0 +set sp_coop 1       (then start or load a mission)
 *   Co-op join      +set com_holomatch 0 +set coop_connect <address>
 *   Holomatch       +set com_holomatch 1                      (the server browser)
 *
 * Joining needs the host's address (the host's IP, or ip:port). It is typed on the number pad on
 * this screen, not into a text field: the Quest's system keyboard hung the app when a text field
 * was tapped, and an address is only digits, dots and a colon. "Search local network" asks the
 * LAN for co-op hosts (CoopLan, the desktop launchers' query); a tap on a game in the list joins it.
 * The last address is kept. Nothing else is configured here: the game's own menus have the rest.
 */
public class StartActivity extends Activity {
    private static final String TAG = "AwayTeamEF";
    private static final String PREFS = "start";
    private static final String PREF_ADDRESS = "address";
    private static final int MAX_ADDRESS = 21;     // 255.255.255.255:65535

    private final StringBuilder address = new StringBuilder();
    private TextView addressView;
    private TextView status;
    private TextView searchStatus;
    private Button searchButton;
    private LinearLayout results;
    private volatile boolean searching;
    private float d;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Log.i(TAG, "StartActivity onCreate");
        d = getResources().getDisplayMetrics().density;
        final int pad = (int) (24 * d);
        address.append(getSharedPreferences(PREFS, MODE_PRIVATE).getString(PREF_ADDRESS, ""));

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER_HORIZONTAL);
        root.setBackgroundColor(Color.rgb(8, 10, 20));
        root.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText("AwayTeamEF VR");
        title.setTextColor(Color.rgb(255, 153, 0));
        title.setTextSize(30);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setGravity(Gravity.CENTER);
        root.addView(title, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        status = new TextView(this);
        status.setTextSize(16);
        status.setGravity(Gravity.CENTER);
        status.setPadding(0, (int) (6 * d), 0, (int) (12 * d));
        root.addView(status, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        // two columns: the modes on the left, joining a co-op game on the right
        LinearLayout columns = new LinearLayout(this);
        columns.setOrientation(LinearLayout.HORIZONTAL);
        root.addView(columns, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        LinearLayout left = column();
        LinearLayout right = column();
        LinearLayout.LayoutParams half = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        half.rightMargin = pad / 2;
        columns.addView(left, half);
        half = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        half.leftMargin = pad / 2;
        columns.addView(right, half);

        addButton(left, "Single player", Color.rgb(255, 153, 0), 60, new View.OnClickListener() {
            @Override public void onClick(View v) { launch("+set com_holomatch 0"); }
        });
        addButton(left, "Co-op host", Color.rgb(204, 153, 255), 60, new View.OnClickListener() {
            @Override public void onClick(View v) { launch("+set com_holomatch 0 +set sp_coop 1"); }
        });
        addButton(left, "Holomatch", Color.rgb(255, 153, 102), 60, new View.OnClickListener() {
            @Override public void onClick(View v) { launch("+set com_holomatch 1"); }
        });
        searchButton = addButton(left, "Search local network", Color.rgb(153, 255, 204), 64, new View.OnClickListener() {
            @Override public void onClick(View v) { search(); }
        });
        searchStatus = new TextView(this);
        searchStatus.setTextSize(15);
        searchStatus.setTextColor(Color.rgb(170, 175, 200));
        searchStatus.setPadding(0, (int) (6 * d), 0, (int) (4 * d));
        left.addView(searchStatus);
        results = new LinearLayout(this);
        results.setOrientation(LinearLayout.VERTICAL);
        left.addView(results, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));


        label(right, "Join a co-op game");
        addressView = new TextView(this);
        addressView.setTextSize(24);
        addressView.setTypeface(Typeface.MONOSPACE);
        addressView.setTextColor(Color.WHITE);
        addressView.setBackgroundColor(Color.rgb(28, 32, 52));
        addressView.setGravity(Gravity.CENTER_VERTICAL);
        addressView.setPadding((int) (12 * d), 0, (int) (12 * d), 0);
        right.addView(addressView, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, (int) (52 * d)));
        showAddress();

        // the number pad: the host's address is digits, dots and a colon
        addKeys(right, new String[]{"1", "2", "3", "4", "5", "6"});
        addKeys(right, new String[]{"7", "8", "9", "0", ".", ":"});
        LinearLayout row = keyRow(right);
        addKey(row, "Back", new View.OnClickListener() {
            @Override public void onClick(View v) {
                if (address.length() > 0) address.setLength(address.length() - 1);
                showAddress();
            }
        });
        addKey(row, "Clear", new View.OnClickListener() {
            @Override public void onClick(View v) { address.setLength(0); showAddress(); }
        });
        addButton(right, "Co-op join", Color.rgb(153, 204, 255), 64, new View.OnClickListener() {
            @Override public void onClick(View v) { join(address.toString().trim()); }
        });
        // a small panel scrolls instead of cutting the lower rows off
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(Color.rgb(8, 10, 20));
        scroll.addView(root, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        setContentView(scroll);
    }

    @Override
    protected void onResume() {
        super.onResume();
        File paks = new File(getObbDir(), "baseEF/pak0.pk3");
        if (paks.isFile()) {
            status.setTextColor(Color.rgb(160, 200, 160));
            status.setText("Game data found");
        } else {
            status.setTextColor(Color.rgb(255, 120, 100));
            status.setText("Game data missing: copy pak0 to pak3 into " + getObbDir() + "/baseEF/");
        }
    }

    private LinearLayout column() {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        return c;
    }

    private void label(LinearLayout parent, String text) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextSize(18);
        t.setTextColor(Color.rgb(170, 175, 200));
        t.setPadding(0, 0, 0, (int) (4 * d));
        parent.addView(t);
    }

    private Button addButton(LinearLayout parent, String text, int colour, int heightDp, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextSize(24);
        b.setTextColor(Color.BLACK);
        b.setBackgroundColor(colour);
        b.setOnClickListener(l);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, (int) (heightDp * d));
        lp.topMargin = (int) (10 * d);
        parent.addView(b, lp);
        return b;
    }

    private LinearLayout keyRow(LinearLayout parent) {
        LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, (int) (48 * d));
        lp.topMargin = (int) (6 * d);
        parent.addView(r, lp);
        return r;
    }

    private void addKey(LinearLayout row, final String text, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextSize(22);
        b.setTextColor(Color.WHITE);
        b.setBackgroundColor(Color.rgb(48, 54, 84));
        b.setOnClickListener(l);
        b.setPadding(0, 0, 0, 0);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f);
        lp.leftMargin = lp.rightMargin = (int) (3 * d);
        row.addView(b, lp);
    }

    private void addKeys(LinearLayout parent, String[] keys) {
        LinearLayout row = keyRow(parent);
        for (final String k : keys) {
            addKey(row, k, new View.OnClickListener() {
                @Override public void onClick(View v) {
                    if (address.length() < MAX_ADDRESS) address.append(k);
                    showAddress();
                }
            });
        }
    }

    private void showAddress() {
        if (address.length() == 0) {
            addressView.setTextColor(Color.rgb(120, 120, 140));
            addressView.setText("Host address");
        } else {
            addressView.setTextColor(Color.WHITE);
            addressView.setText(address.toString());
        }
    }

    /** Asks the LAN for co-op hosts on a worker thread (the UI thread only shows the answer). */
    private void search() {
        if (searching) return;
        searching = true;
        searchButton.setEnabled(false);
        results.removeAllViews();
        searchStatus.setText("Searching the local network...");
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
        if (isFinishing() || isDestroyed()) return;
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
            Button b = new Button(this);
            b.setAllCaps(false);
            b.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
            b.setTextSize(16);
            b.setBackgroundColor(problem.isEmpty() ? Color.rgb(36, 60, 56) : Color.rgb(46, 40, 44));
            b.setTextColor(problem.isEmpty() ? Color.WHITE : Color.rgb(170, 160, 165));
            b.setText(g.title() + "\n" + (problem.isEmpty() ? g.detail() : problem + " \u00b7 " + g.address));
            b.setOnClickListener(new View.OnClickListener() {
                @Override public void onClick(View v) {
                    address.setLength(0);
                    address.append(g.address);
                    showAddress();
                    if (problem.isEmpty()) join(g.address);
                    else Toast.makeText(StartActivity.this, problem, Toast.LENGTH_LONG).show();
                }
            });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.topMargin = (int) (6 * d);
            results.addView(b, lp);
        }
    }

    private void join(String addr) {
        // the address goes on the engine's command line: digits, dots and a colon only (as the
        // desktop launchers accept)
        if (addr.isEmpty() || !addr.matches("[0-9.:]+")) {
            Toast.makeText(this, "Type the host's address first (numbers and dots, e.g. 192.168.1.20)",
                    Toast.LENGTH_LONG).show();
            return;
        }
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(PREF_ADDRESS, addr).apply();
        launch("+set com_holomatch 0 +set coop_connect " + addr);
    }

    private void launch(String engineArgs) {
        Log.i(TAG, "StartActivity launching: " + engineArgs);
        Intent i = new Intent(this, QuestActivity.class);
        i.putExtra("args", engineArgs);
        startActivity(i);
        finish();
    }
}
