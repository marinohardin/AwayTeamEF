package org.awayteamef.app;

import android.app.Activity;
import android.content.res.ColorStateList;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.PorterDuff;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.InputStream;

/**
 * The Quest start screen's look: the desktop launchers' dark style (group captions in capitals over
 * rounded boxes, the system blue accent, green/orange status), with big pointer targets.
 *
 * Nothing here has a fixed height: buttons and labels are WRAP_CONTENT with a minimum height and
 * padding, so text never clips whatever the panel's density or font scale (the old start screen's
 * fixed 64 dp buttons cut "Search local network" off at the bottom on the headset).
 */
final class Ui {
    static final int BG = Color.rgb(30, 30, 32);
    static final int FILL = Color.argb(13, 255, 255, 255);
    static final int STROKE = Color.rgb(58, 58, 60);
    static final int TEXT = Color.rgb(242, 242, 247);
    static final int SECONDARY = Color.rgb(152, 152, 159);
    static final int TERTIARY = Color.rgb(99, 99, 104);
    static final int ACCENT = Color.rgb(10, 132, 255);
    static final int GREEN = Color.rgb(48, 209, 88);
    static final int ORANGE = Color.rgb(255, 159, 10);
    static final int RED = Color.rgb(255, 69, 58);
    static final int GOLD = Color.rgb(0xD6, 0xA0, 0x3C);
    static final int PURPLE = Color.rgb(191, 90, 242);
    static final int AMBER = Color.rgb(255, 214, 10);
    static final int BUTTON = Color.rgb(58, 58, 62);
    static final int FIELD = Color.rgb(17, 17, 19);

    /** Quake 3 colour codes ^0..^7 on a dark background (^0 black shown grey so it stays readable). */
    static final int[] Q3 = {
        Color.rgb(140, 140, 140), Color.rgb(255, 69, 58), Color.rgb(48, 209, 88), Color.rgb(255, 214, 10),
        Color.rgb(64, 156, 255), Color.rgb(100, 210, 255), Color.rgb(255, 55, 210), Color.rgb(242, 242, 247)
    };

    final Activity a;
    final float d;

    Ui(Activity a) {
        this.a = a;
        d = a.getResources().getDisplayMetrics().density;
    }

    int dp(float v) { return Math.round(v * d); }

    static GradientDrawable round(int fill, float radiusPx, int strokePx, int stroke) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(fill);
        g.setCornerRadius(radiusPx);
        if (strokePx > 0) g.setStroke(strokePx, stroke);
        return g;
    }

    /** Normal, hovered (the controller pointer), pressed and disabled looks. */
    Drawable buttonBackground(int colour) {
        StateListDrawable s = new StateListDrawable();
        float r = dp(12);
        s.addState(new int[]{-android.R.attr.state_enabled}, round(Color.rgb(44, 44, 46), r, 0, 0));
        s.addState(new int[]{android.R.attr.state_pressed}, round(blend(colour, Color.WHITE, 0.30f), r, 0, 0));
        s.addState(new int[]{android.R.attr.state_hovered}, round(blend(colour, Color.WHITE, 0.16f), r, dp(2), Color.argb(150, 255, 255, 255)));
        s.addState(new int[]{android.R.attr.state_focused}, round(blend(colour, Color.WHITE, 0.16f), r, dp(2), Color.argb(150, 255, 255, 255)));
        s.addState(new int[]{}, round(colour, r, 0, 0));
        return s;
    }

    static int blend(int c, int with, float t) {
        return Color.rgb(
                Math.round(Color.red(c) + (Color.red(with) - Color.red(c)) * t),
                Math.round(Color.green(c) + (Color.green(with) - Color.green(c)) * t),
                Math.round(Color.blue(c) + (Color.blue(with) - Color.blue(c)) * t));
    }

    TextView text(String s, float sp, int colour) {
        TextView t = new TextView(a);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(colour);
        t.setIncludeFontPadding(true);
        return t;
    }

    TextView bold(String s, float sp, int colour) {
        TextView t = text(s, sp, colour);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    /** A button: a TextView with padding and a minimum height, never a fixed height. */
    TextView button(String s, boolean primary, View.OnClickListener l) {
        TextView b = text(s, 20, primary ? Color.WHITE : TEXT);
        if (primary) b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setGravity(Gravity.CENTER);
        b.setMinHeight(dp(60));
        b.setMinWidth(dp(120));
        b.setPadding(dp(22), dp(12), dp(22), dp(12));
        b.setBackground(buttonBackground(primary ? ACCENT : BUTTON));
        b.setTextColor(new ColorStateList(new int[][]{{-android.R.attr.state_enabled}, {}},
                new int[]{TERTIARY, primary ? Color.WHITE : TEXT}));
        b.setClickable(true);
        b.setFocusable(true);
        b.setOnClickListener(l);
        return b;
    }

    TextView smallButton(String s, View.OnClickListener l) {
        TextView b = button(s, false, l);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
        b.setMinHeight(dp(54));
        b.setMinWidth(dp(100));
        b.setPadding(dp(18), dp(10), dp(18), dp(10));
        return b;
    }

    LinearLayout column() {
        LinearLayout c = new LinearLayout(a);
        c.setOrientation(LinearLayout.VERTICAL);
        return c;
    }

    LinearLayout row() {
        LinearLayout r = new LinearLayout(a);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        return r;
    }

    /** The desktop's group: a caption in capitals above a rounded box. Returns the box. */
    LinearLayout group(LinearLayout parent, String caption) {
        TextView c = bold(caption.toUpperCase(), 14, SECONDARY);
        c.setLetterSpacing(0.04f);
        c.setPadding(dp(2), dp(4), 0, dp(6));
        parent.addView(c);
        LinearLayout box = column();
        box.setBackground(round(FILL, dp(16), dp(1.5f), STROKE));
        box.setPadding(dp(18), dp(14), dp(18), dp(14));
        LinearLayout.LayoutParams lp = matchWrap();
        lp.bottomMargin = dp(14);
        parent.addView(box, lp);
        return box;
    }

    LinearLayout card(LinearLayout parent) {
        LinearLayout box = row();
        box.setBackground(round(FILL, dp(18), dp(1.5f), STROKE));
        box.setPadding(dp(20), dp(16), dp(20), dp(16));
        LinearLayout.LayoutParams lp = matchWrap();
        lp.bottomMargin = dp(14);
        parent.addView(box, lp);
        return box;
    }

    static LinearLayout.LayoutParams matchWrap() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    static LinearLayout.LayoutParams wrap() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    static LinearLayout.LayoutParams weight(float w) {
        return new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, w);
    }

    LinearLayout.LayoutParams gapLeft(LinearLayout.LayoutParams lp, float gapDp) {
        lp.leftMargin = dp(gapDp);
        return lp;
    }

    LinearLayout.LayoutParams gapTop(LinearLayout.LayoutParams lp, float gapDp) {
        lp.topMargin = dp(gapDp);
        return lp;
    }

    /** A status line: a coloured mark and secondary text that wraps (never truncated). */
    LinearLayout status(String mark, int markColour, String s) {
        LinearLayout r = row();
        r.setGravity(Gravity.TOP);
        TextView m = text(mark, 17, markColour);
        m.setPadding(0, 0, dp(8), 0);
        r.addView(m);
        TextView t = text(s, 17, SECONDARY);
        r.addView(t, weight(1));
        r.setPadding(0, dp(5), 0, 0);
        return r;
    }

    TextView chip(String s, int fg, int bg) {
        TextView t = text(s, 15, fg);
        t.setBackground(round(bg, dp(9), 0, 0));
        t.setPadding(dp(11), dp(5), dp(11), dp(5));
        return t;
    }

    /**
     * The header's title lettering: a white-on-transparent picture of the text (assets/launcher, from
     * tools/launcher-titles/png at 3 px per point), tinted like the desktop launchers tint it.
     * scale: points to dp.
     */
    View title(String asset, int tint, float scale, String description) {
        try (InputStream in = a.getAssets().open("launcher/" + asset)) {
            Bitmap b = BitmapFactory.decodeStream(in);
            ImageView v = new ImageView(a);
            v.setImageBitmap(b);
            v.setScaleType(ImageView.ScaleType.FIT_XY);
            v.setColorFilter(tint, PorterDuff.Mode.SRC_IN);
            v.setContentDescription(description);
            v.setLayoutParams(new LinearLayout.LayoutParams(dp(b.getWidth() / 3f * scale), dp(b.getHeight() / 3f * scale)));
            return v;
        } catch (Exception e) {
            // no picture (a test build without assets): the plain text, never a font file
            TextView t = bold(description, 20 * scale / 1.5f, tint);
            t.setSingleLine(false);
            return t;
        }
    }

    View icon(float sizeDp) {
        try (InputStream in = a.getAssets().open("launcher/icon.png")) {
            ImageView v = new ImageView(a);
            v.setImageBitmap(BitmapFactory.decodeStream(in));
            v.setLayoutParams(new LinearLayout.LayoutParams(dp(sizeDp), dp(sizeDp)));
            return v;
        } catch (Exception e) {
            View v = new View(a);
            v.setLayoutParams(new LinearLayout.LayoutParams(dp(sizeDp), dp(sizeDp)));
            return v;
        }
    }

    /** Text with Quake 3 colour codes (^0..^9, letters too, as the game reads them) drawn in colour. */
    static CharSequence colourText(String raw) {
        android.text.SpannableStringBuilder sb = new android.text.SpannableStringBuilder();
        int colour = 7;
        for (int i = 0; i < raw.length(); i++) {
            char c = raw.charAt(i);
            if (c == '^' && i + 1 < raw.length() && Character.isLetterOrDigit(raw.charAt(i + 1)) && raw.charAt(i + 1) < 128) {
                colour = (raw.charAt(i + 1) - '0') & 7;
                i++;
                continue;
            }
            int start = sb.length();
            sb.append(c);
            sb.setSpan(new android.text.style.ForegroundColorSpan(Q3[colour]), start, sb.length(), 0);
        }
        return sb;
    }

    /** Server names from the core: name plus one colour digit per character. */
    static CharSequence colourText(String name, String colours) {
        android.text.SpannableStringBuilder sb = new android.text.SpannableStringBuilder(name);
        for (int i = 0; i < name.length() && i < colours.length(); i++) {
            int c = colours.charAt(i) - '0';
            if (c < 0 || c > 7) c = 7;
            sb.setSpan(new android.text.style.ForegroundColorSpan(Q3[c]), i, i + 1, 0);
        }
        return sb;
    }

    static String ellipsize(String s, int max) {
        return s.length() <= max ? s : s.substring(0, max - 1) + "…";
    }

    static boolean empty(String s) { return TextUtils.isEmpty(s); }
}
