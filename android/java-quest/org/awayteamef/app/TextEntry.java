package org.awayteamef.app;

import android.graphics.Color;
import android.graphics.Typeface;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * An on-panel keyboard page: the player name, a password, or an address. The Quest's system
 * keyboard is never opened (a text field that summoned it hung the old start screen), so every
 * key is a big button on the panel itself, like the co-op number pad.
 */
final class TextEntry {
    enum Mode { NAME, PASSWORD, ADDRESS }

    interface Done { void done(String text); }

    private final Ui ui;
    private final Mode mode;
    private final int max;
    private final StringBuilder text;
    private final TextView field, count, preview;
    private final LinearLayout keys;
    private boolean shift, symbols, colours, reveal;

    // the punctuation a player name may hold: ASCII 32..126 without " + ; \ % (lc_clean_name);
    // passwords take the same set without the colour key
    private static final String[] SYMBOLS1 = {"!", "#", "$", "&", "'", "(", ")", "*", ",", "-"};
    private static final String[] SYMBOLS2 = {".", "/", ":", "<", "=", ">", "?", "@", "[", "]"};
    private static final String[] SYMBOLS3 = {"_", "`", "{", "|", "}", "~", "^"};

    /**
     * @param back  the Cancel button's action; done gets the text on Done.
     */
    static View page(Ui ui, String title, String hint, String initial, Mode mode, int max,
                     final Runnable back, final Done done) {
        return new TextEntry(ui, title, hint, initial, mode, max, back, done).root;
    }

    private final LinearLayout root;

    private TextEntry(final Ui ui, String title, String hint, String initial, Mode mode, int max,
                      final Runnable back, final Done done) {
        this.ui = ui;
        this.mode = mode;
        this.max = max;
        text = new StringBuilder(initial == null ? "" : initial);
        root = ui.column();

        LinearLayout top = ui.row();
        top.addView(ui.button("‹ Cancel", false, new View.OnClickListener() {
            @Override public void onClick(View v) { back.run(); }
        }));
        TextView t = ui.bold(title, 28, Ui.TEXT);
        t.setPadding(ui.dp(16), 0, 0, 0);
        top.addView(t, Ui.weight(1));
        if (mode == Mode.PASSWORD) {
            top.addView(ui.smallButton("Show", new View.OnClickListener() {
                @Override public void onClick(View v) {
                    reveal = !reveal;
                    ((TextView) v).setText(reveal ? "Hide" : "Show");
                    show();
                }
            }));
        }
        root.addView(top);

        LinearLayout box = ui.row();
        box.setBackground(Ui.round(Ui.FIELD, ui.dp(12), ui.dp(1.5f), Color.rgb(72, 72, 74)));
        box.setPadding(ui.dp(18), ui.dp(10), ui.dp(18), ui.dp(10));
        box.setMinimumHeight(ui.dp(72));
        field = ui.text("", 30, Ui.TEXT);
        if (mode == Mode.ADDRESS) field.setTypeface(Typeface.MONOSPACE);
        box.addView(field, Ui.weight(1));
        count = ui.text("", 16, Ui.SECONDARY);
        box.addView(count);
        root.addView(box, ui.gapTop(Ui.matchWrap(), 14));

        preview = ui.text("", 18, Ui.SECONDARY);
        preview.setPadding(ui.dp(4), ui.dp(6), 0, 0);
        root.addView(preview);
        if (hint != null) {
            TextView h = ui.text(hint, 17, Ui.SECONDARY);
            h.setPadding(ui.dp(4), ui.dp(4), 0, 0);
            root.addView(h);
        }

        keys = ui.column();
        root.addView(keys, ui.gapTop(Ui.matchWrap(), 10));
        buildKeys();
        LinearLayout bottom = ui.row();
        bottom.setGravity(Gravity.END);
        bottom.addView(ui.button("Done", true, new View.OnClickListener() {
            @Override public void onClick(View v) { done.done(text.toString()); }
        }), new LinearLayout.LayoutParams(ui.dp(220), LinearLayout.LayoutParams.WRAP_CONTENT));
        root.addView(bottom, ui.gapTop(Ui.matchWrap(), 10));
        show();
    }

    private void show() {
        String s = text.toString();
        if (mode == Mode.PASSWORD && !reveal) {
            StringBuilder dots = new StringBuilder();
            for (int i = 0; i < s.length(); i++) dots.append('•');
            field.setText(dots.length() == 0 ? "" : dots + "│");
        } else {
            field.setText(s + "│");
        }
        count.setText(s.length() + " of " + max);
        if (mode == Mode.NAME && s.indexOf('^') >= 0) {
            android.text.SpannableStringBuilder sb = new android.text.SpannableStringBuilder("Shown as: ");
            sb.append(Ui.colourText(s));
            preview.setText(sb);
            preview.setVisibility(View.VISIBLE);
        } else {
            // keeps its room for a name (the keys don't move when a colour code is typed)
            preview.setText(" ");
            preview.setVisibility(mode == Mode.NAME ? View.INVISIBLE : View.GONE);
        }
    }

    private void type(String k) {
        if (text.length() + k.length() <= max) text.append(k);
        if (shift && !symbols && !colours) {
            shift = false;
            buildKeys();
        }
        show();
    }

    private void buildKeys() {
        keys.removeAllViews();
        if (mode == Mode.ADDRESS) {
            keyRow(new String[]{"1", "2", "3", "4", "5", "6"});
            keyRow(new String[]{"7", "8", "9", "0", ".", ":"});
            LinearLayout r = row();
            fnKey(r, "⌫ Back", 1, new View.OnClickListener() {
                @Override public void onClick(View v) { backspace(); }
            });
            fnKey(r, "Clear", 1, new View.OnClickListener() {
                @Override public void onClick(View v) { text.setLength(0); show(); }
            });
            return;
        }
        if (colours) {
            LinearLayout r = row();
            for (int i = 0; i < 8; i++) {
                final String code = "^" + i;
                TextView k = key(r, code, 1);
                k.setTextColor(Ui.Q3[i]);
                k.setOnClickListener(new View.OnClickListener() {
                    @Override public void onClick(View v) { type(code); }
                });
            }
            TextView note = ui.text("A colour code colours the letters after it; ^7 is white again.", 17, Ui.SECONDARY);
            note.setPadding(ui.dp(4), ui.dp(10), 0, ui.dp(10));
            keys.addView(note);
        } else if (symbols) {
            keyRow(SYMBOLS1);
            keyRow(SYMBOLS2);
            keyRow(mode == Mode.NAME ? SYMBOLS3 : new String[]{"_", "`", "{", "|", "}", "~", "^"});
        } else {
            keyRow(new String[]{"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"});
            keyRow(letters("qwertyuiop"));
            keyRow(letters("asdfghjkl-"));
            LinearLayout r = row();
            fnKey(r, shift ? "⇧ SHIFT" : "⇧ Shift", 1.5f, new View.OnClickListener() {
                @Override public void onClick(View v) { shift = !shift; buildKeys(); }
            });
            for (final String k : letters("zxcvbnm.")) {
                key(r, k, 1).setOnClickListener(new View.OnClickListener() {
                    @Override public void onClick(View v) { type(k); }
                });
            }
            fnKey(r, "⌫", 1.5f, new View.OnClickListener() {
                @Override public void onClick(View v) { backspace(); }
            });
        }
        LinearLayout r = row();
        fnKey(r, symbols ? "abc" : "#?! Symbols", 2, new View.OnClickListener() {
            @Override public void onClick(View v) { symbols = !symbols; colours = false; buildKeys(); }
        });
        if (mode == Mode.NAME) {
            fnKey(r, colours ? "abc" : "^ Colours", 2, new View.OnClickListener() {
                @Override public void onClick(View v) { colours = !colours; symbols = false; buildKeys(); }
            });
        }
        fnKey(r, "Space", 3, new View.OnClickListener() {
            @Override public void onClick(View v) { type(" "); }
        });
        if (symbols || colours) {
            fnKey(r, "⌫", 1, new View.OnClickListener() {
                @Override public void onClick(View v) { backspace(); }
            });
        }
        fnKey(r, "Clear", 1.3f, new View.OnClickListener() {
            @Override public void onClick(View v) { text.setLength(0); show(); }
        });
    }

    private void backspace() {
        if (text.length() > 0) text.setLength(text.length() - 1);
        show();
    }

    private String[] letters(String s) {
        String[] k = new String[s.length()];
        for (int i = 0; i < k.length; i++) {
            String c = s.substring(i, i + 1);
            k[i] = shift ? c.toUpperCase() : c;
        }
        return k;
    }

    private LinearLayout row() {
        LinearLayout r = ui.row();
        keys.addView(r, ui.gapTop(Ui.matchWrap(), 8));
        return r;
    }

    private void keyRow(String[] ks) {
        LinearLayout r = row();
        for (final String k : ks) {
            key(r, k, 1).setOnClickListener(new View.OnClickListener() {
                @Override public void onClick(View v) { type(k); }
            });
        }
    }

    private TextView key(LinearLayout r, String label, float weight) {
        TextView k = ui.text(label, 24, Ui.TEXT);
        k.setGravity(Gravity.CENTER);
        k.setMinHeight(ui.dp(64));
        k.setPadding(0, ui.dp(8), 0, ui.dp(8));
        k.setBackground(ui.buttonBackground(Color.rgb(47, 47, 51)));
        k.setClickable(true);
        LinearLayout.LayoutParams lp = Ui.weight(weight);
        lp.leftMargin = lp.rightMargin = ui.dp(4);
        r.addView(k, lp);
        return k;
    }

    private void fnKey(LinearLayout r, String label, float weight, View.OnClickListener l) {
        TextView k = key(r, label, weight);
        k.setTextSize(19);
        k.setBackground(ui.buttonBackground(Color.rgb(61, 61, 66)));
        k.setOnClickListener(l);
    }
}
