package org.awayteamef.app;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;

/**
 * The Quest build's start screen (AndroidManifest-quest.xml; only compiled by make-apk.sh VR=1).
 *
 * A plain Android activity, shown as a flat panel in the headset before the game starts: four
 * buttons that launch QuestActivity (the VR game activity) with the engine arguments that mode
 * needs, the same ones the desktop launchers put on the command line (efcode/launcher/lc_launch.c):
 *
 *   Single player   +set com_holomatch 0
 *   Co-op host      +set com_holomatch 0 +set sp_coop 1       (then start or load a mission)
 *   Co-op join      +set com_holomatch 0 +set coop_connect <address>
 *   Holomatch       +set com_holomatch 1                      (the server browser)
 *
 * Joining needs the host's address (the host's IP, or ip:port); the field above the buttons keeps
 * the last one. Nothing else is configured here: the game's own menus have the rest.
 */
public class StartActivity extends Activity {
    private static final String PREFS = "start";
    private static final String PREF_ADDRESS = "address";

    private EditText addressField;
    private TextView status;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        final float d = getResources().getDisplayMetrics().density;
        final int pad = (int) (24 * d);

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

        addButton(root, "Single player", Color.rgb(255, 153, 0), new View.OnClickListener() {
            @Override public void onClick(View v) { launch("+set com_holomatch 0"); }
        });
        addButton(root, "Co-op host", Color.rgb(204, 153, 255), new View.OnClickListener() {
            @Override public void onClick(View v) { launch("+set com_holomatch 0 +set sp_coop 1"); }
        });

        addressField = new EditText(this);
        addressField.setHint("Host address for Co-op join, e.g. 192.168.1.20");
        addressField.setHintTextColor(Color.rgb(120, 120, 140));
        addressField.setTextColor(Color.WHITE);
        addressField.setSingleLine(true);
        addressField.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        addressField.setText(getSharedPreferences(PREFS, MODE_PRIVATE).getString(PREF_ADDRESS, ""));
        addressField.setTextSize(20);
        root.addView(addressField, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        addButton(root, "Co-op join", Color.rgb(153, 204, 255), new View.OnClickListener() {
            @Override public void onClick(View v) { join(); }
        });
        addButton(root, "Holomatch", Color.rgb(255, 153, 102), new View.OnClickListener() {
            @Override public void onClick(View v) { launch("+set com_holomatch 1"); }
        });

        setContentView(root);
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

    private void addButton(LinearLayout root, String text, int colour, View.OnClickListener l) {
        final float d = getResources().getDisplayMetrics().density;
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextSize(24);
        b.setTextColor(Color.BLACK);
        b.setBackgroundColor(colour);
        b.setOnClickListener(l);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, (int) (64 * d));
        lp.topMargin = (int) (10 * d);
        root.addView(b, lp);
    }

    private void join() {
        String address = addressField.getText().toString().trim();
        // the address goes on the engine's command line: digits, dots and a colon only (as the
        // desktop launchers accept)
        if (address.isEmpty() || !address.matches("[0-9.:]+")) {
            Toast.makeText(this, "Type the host's address first (numbers and dots, e.g. 192.168.1.20)",
                    Toast.LENGTH_LONG).show();
            addressField.requestFocus();
            return;
        }
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(PREF_ADDRESS, address).apply();
        launch("+set com_holomatch 0 +set coop_connect " + address);
    }

    private void launch(String engineArgs) {
        Intent i = new Intent(this, QuestActivity.class);
        i.putExtra("args", engineArgs);
        startActivity(i);
        finish();
    }
}
