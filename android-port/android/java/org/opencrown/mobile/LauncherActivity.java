package org.opencrown.mobile;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.BitmapDrawable;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.LayerDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.inputmethod.EditorInfo;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;

// The app's entry point (like the PC launcher): checks the user's dump in Internal storage/OpenCrown/dump/, manages
// saves (Import PC save) and touch-control settings, then starts the game activity with the resolved paths and
// closes. Everything the game uses lives in that one folder, RetroArch style: the user copies the dump there once.
public class LauncherActivity extends Activity {
    static final String TITLE_ID = "PCSE00019";
    static final String EXTRA_GAME_FAILED = "org.opencrown.mobile.GAME_FAILED";  // OpenCrownActivity: main() returned

    static final int GOLD = 0xffe2b15a, TEXT = 0xffe8dccb, DIM = 0xffa89a86, OK = 0xff8fc46a, BAD = 0xffd9785f;
    static final int PANEL = 0xd9171310, SIDEBAR = 0xd91c1712, SELECTED = 0xff6b4a1f, BG = 0xff120e0b;

    File root;                 // Internal storage/OpenCrown
    DumpCheck dump;            // last scan
    int page = 0;              // 0 game, 1 save folder, 2 controls, 3 display, 4 online, 5 report a bug, 6 update
    LinearLayout content;
    ScrollView scroll;
    TextView status;
    Button play;
    final List<TextView> tabs = new ArrayList<>();
    ImageView icon;
    FrameLayout frame;
    String lastFailure;        // why the last start failed (status bar), null = no failure

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        if (resumeGame()) return;
        root = new File(Environment.getExternalStorageDirectory(), "OpenCrown");
        buildUi();
        startUpdates();
        if (saved == null && getIntent().getBooleanExtra(EXTRA_GAME_FAILED, false)) showGameFailed();
    }

    // --- failed start ----------------------------------------------------------------------------------------------
    // The game stopped before it ran: say why (the loader's "mobile_run: <reason>" line in log.txt) and show the end
    // of the log, rather than leaving the player on the home screen with nothing.
    void showGameFailed() {
        List<String> tail = logTail(logFile(), 40);
        String reason = null;
        for (String l : tail) if (l.startsWith("mobile_run: ")) reason = l.substring("mobile_run: ".length());
        lastFailure = reason != null ? reason : "it stopped before the first frame";
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(20), dp(8), dp(20), 0);
        box.addView(text(reason != null ? capitalize(reason) + "."
                : "It stopped before the first frame. The end of the log below may say why.", 15, TEXT));
        TextView log = text(tail.isEmpty() ? "(log.txt is empty)" : String.join("\n", tail), 11, DIM);
        log.setTypeface(Typeface.MONOSPACE);
        log.setTextIsSelectable(true);
        log.setPadding(0, dp(10), 0, dp(4));
        ScrollView s = new ScrollView(this);
        s.addView(log);
        box.addView(s, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(180)));
        box.addView(text("Full log: " + logFile().getAbsolutePath(), 11, DIM));
        new android.app.AlertDialog.Builder(this)
                .setTitle("The game couldn't start").setView(box)
                .setPositiveButton("Close", null)
                .setNeutralButton("Report a bug", new android.content.DialogInterface.OnClickListener() {
                    public void onClick(android.content.DialogInterface d, int w) { page = 5; refresh(); }
                })
                .show();
        refresh();
    }

    static String capitalize(String s) {
        return s.isEmpty() ? s : Character.toUpperCase(s.charAt(0)) + s.substring(1);
    }

    // Last n lines of a text file (reads at most its last 64 KB); empty when missing.
    static List<String> logTail(File f, int n) {
        List<String> lines = new ArrayList<>();
        try (java.io.RandomAccessFile in = new java.io.RandomAccessFile(f, "r")) {
            long len = in.length(), from = Math.max(0, len - 65536);
            byte[] b = new byte[(int) (len - from)];
            in.seek(from);
            in.readFully(b);
            String[] all = new String(b, java.nio.charset.StandardCharsets.UTF_8).split("\r?\n");
            for (int i = Math.max(from > 0 ? 1 : 0, all.length - n); i < all.length; i++) lines.add(all[i]);
        } catch (IOException e) {
            // no log yet
        }
        return lines;
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (updReceiver != null) unregisterReceiver(updReceiver);
        updCancel.set(true);
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemBars();
        refresh();  // back from the permission screen, or the user copied files in meanwhile
    }

    @Override
    protected void onPause() {
        super.onPause();
        handler.removeCallbacks(friendsTick);  // the Online page re-arms it in onResume -> refresh
    }

    @Override
    public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus) hideSystemBars();
    }

    void hideSystemBars() {
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        }
    }

    // --- storage access --------------------------------------------------------------------------------------------
    boolean haveAccess() {
        if (Build.VERSION.SDK_INT >= 30) return Environment.isExternalStorageManager();
        return checkSelfPermission(android.Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED
                && checkSelfPermission(android.Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    void requestAccess() {
        if (Build.VERSION.SDK_INT >= 30) {
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName())));
            } catch (Exception e) {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        } else {
            requestPermissions(new String[] {android.Manifest.permission.READ_EXTERNAL_STORAGE,
                    android.Manifest.permission.WRITE_EXTERNAL_STORAGE}, 1);
        }
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] results) {
        refresh();
    }

    // The folder layout + a README telling the user where the dump goes (created once access is granted).
    void prepareFolders() {
        new File(root, "dump/Game").mkdirs();
        new File(root, "dump/Update").mkdirs();
        new File(root, "saves").mkdirs();
        File readme = new File(root, "README.txt");
        if (readme.exists()) return;
        String text = "OpenCrown Mobile\r\n\r\n"
                + "Copy your own PS Vita dump of Dragon's Crown (US, " + TITLE_ID + ") here:\r\n"
                + "  dump/Game/    the base game (eboot.bin, DRACRO.CPK, sce_module/, sce_sys/ ...)\r\n"
                + "  dump/Update/  the 1.09 update (eboot.bin, DRACRO_PATCH.CPK, DRACRO_PATCH_US.CPK, sce_module/, sce_sys/ ...)\r\n\r\n"
                + "The dump must be decrypted. saves/ holds your save data, input.ini the controls, log.txt the\r\n"
                + "last run's log (attach it to bug reports).\r\n";
        try (FileOutputStream o = new FileOutputStream(readme)) {
            o.write(text.getBytes("UTF-8"));
        } catch (IOException ignored) {
        }
    }

    // --- dump check ------------------------------------------------------------------------------------------------
    static class Part {
        File dir;
        String version = "", problem;
    }

    static class DumpCheck {
        Part game, update;
        final List<String[]> lines = new ArrayList<>();  // {"ok"|"missing"|"error", text}
        boolean ready() { return game != null && game.problem == null && update != null && update.problem == null; }
    }

    // Finds the base game (CATEGORY gd) and the update (gp) anywhere up to two levels under dump/, so dump/Game +
    // dump/Update, app/PCSE00019 + patch/PCSE00019 (VitaShell layout) or other names all work.
    DumpCheck scan() {
        DumpCheck r = new DumpCheck();
        List<File> dirs = new ArrayList<>();
        collect(new File(root, "dump"), 0, dirs);
        for (File d : dirs) {
            Sfo sfo = Sfo.read(new File(d, "sce_sys/param.sfo"));
            if (sfo == null) continue;
            Part p = new Part();
            p.dir = d;
            p.version = sfo.get("APP_VER");
            String cat = sfo.get("CATEGORY");
            if (!TITLE_ID.equals(sfo.get("TITLE_ID")))
                p.problem = "is " + sfo.get("TITLE_ID") + "; only the US version (" + TITLE_ID + ") is supported";
            else if (new File(d, "sce_pfs").exists())
                p.problem = "is still encrypted (copied without decrypting): dump it decrypted";
            if ("gd".equals(cat) && r.game == null) r.game = p;
            else if ("gp".equals(cat) && (r.update == null || p.version.compareTo(r.update.version) > 0)) r.update = p;
        }
        if (r.game != null && r.game.problem == null) {
            if (!new File(r.game.dir, "eboot.bin").isFile()) r.game.problem = "has no eboot.bin";
            else if (!new File(r.game.dir, "DRACRO.CPK").isFile()) r.game.problem = "has no DRACRO.CPK";
        }
        String why;
        if (r.update != null && r.update.problem == null) {
            if (!new File(r.update.dir, "eboot.bin").isFile()) r.update.problem = "has no eboot.bin";
            else if ((why = selfProblem(new File(r.update.dir, "eboot.bin"))) != null) r.update.problem = why;
            else if (!"01.09".equals(r.update.version)) r.update.problem = "is version " + r.update.version + "; update 1.09 is needed (install it on your Vita and dump it again)";
        }
        describe(r, r.game, "Base game", "dump/Game");
        describe(r, r.update, "Update 1.09", "dump/Update");
        return r;
    }

    // The loader's rule (src/guest/image.cpp): a SELF ("SCE\0") whose loadable segments are flagged encrypted (segment
    // info +0x18 == 1) was copied off the Vita without decrypting; the game would stop at once. A folder without
    // sce_pfs/ can still hold such an eboot.bin. null = fine (decrypted SELF or plain ELF).
    static String selfProblem(File eboot) {
        try (java.io.RandomAccessFile f = new java.io.RandomAccessFile(eboot, "r")) {
            byte[] magic = new byte[4];
            f.readFully(magic);
            if (magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') return null;
            if (magic[0] != 'S' || magic[1] != 'C' || magic[2] != 'E' || magic[3] != 0)
                return "has an eboot.bin that is not a Vita executable (damaged copy?): dump it again";
            long elf = u64(f, 0x40), phdr = u64(f, 0x48), seginfo = u64(f, 0x58);
            f.seek(elf + 0x2c);
            int phnum = Short.reverseBytes(f.readShort()) & 0xffff;
            for (int i = 0; i < phnum; i++) {
                f.seek(phdr + i * 0x20L);
                int type = Integer.reverseBytes(f.readInt());  // readX throw at the end of the file (cut short)
                if (type == 1 && u64(f, seginfo + i * 0x20L + 0x18) == 1)
                    return "has an eboot.bin that is still encrypted: dump the game decrypted again";
            }
            return null;
        } catch (IOException e) {
            return "has an eboot.bin that is cut short (incomplete copy): copy it again";
        }
    }

    static long u64(java.io.RandomAccessFile f, long at) throws IOException {
        f.seek(at);
        return Long.reverseBytes(f.readLong());
    }

    void describe(DumpCheck r, Part p, String what, String where) {
        if (p == null) r.lines.add(new String[] {"missing", what + " not found (put it in " + where + ")"});
        else if (p.problem != null) r.lines.add(new String[] {"error", what + " in " + rel(p.dir) + " " + p.problem});
        else r.lines.add(new String[] {"ok", what + " " + p.version + " found (" + rel(p.dir) + ")"});
    }

    String rel(File f) {
        String a = f.getAbsolutePath(), b = root.getAbsolutePath();
        return a.startsWith(b) ? "OpenCrown" + a.substring(b.length()) : a;
    }

    static void collect(File dir, int depth, List<File> out) {
        File[] kids = dir.listFiles();
        if (kids == null) return;
        if (new File(dir, "sce_sys/param.sfo").isFile()) out.add(dir);
        if (depth >= 2) return;
        for (File k : kids)
            if (k.isDirectory() && !k.getName().equals("sce_sys") && !k.getName().equals("sce_module")) collect(k, depth + 1, out);
    }

    // --- UI --------------------------------------------------------------------------------------------------------
    int dp(float v) { return Math.round(v * getResources().getDisplayMetrics().density); }

    static GradientDrawable box(int color, float radius) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(color);
        g.setCornerRadius(radius);
        return g;
    }

    void buildUi() {
        frame = new FrameLayout(this);
        frame.setBackgroundColor(BG);
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setPadding(dp(14), dp(14), dp(14), dp(14));
        frame.addView(row, new FrameLayout.LayoutParams(-1, -1));

        LinearLayout side = new LinearLayout(this);
        side.setOrientation(LinearLayout.VERTICAL);
        side.setBackground(box(SIDEBAR, dp(8)));
        side.setPadding(dp(10), dp(12), dp(10), dp(12));
        row.addView(side, new LinearLayout.LayoutParams(dp(190), -1));
        icon = new ImageView(this);
        icon.setScaleType(ImageView.ScaleType.FIT_CENTER);
        side.addView(icon, new LinearLayout.LayoutParams(-1, dp(80)));
        TextView brand = text("OpenCrown Mobile", 13, DIM);
        brand.setGravity(Gravity.CENTER_HORIZONTAL);
        brand.setPadding(0, dp(6), 0, dp(10));
        side.addView(brand);
        String[] names = {"Game", "Save Folder", "Controls", "Display", "Online", "Report a Bug"};
        for (int i = 0; i < names.length; i++) {
            final int n = i;
            TextView t = text(names[i], 18, TEXT);
            t.setTypeface(Typeface.SERIF, Typeface.BOLD);
            t.setPadding(dp(12), dp(8), dp(12), dp(8));
            t.setFocusable(true);
            t.setClickable(true);
            t.setOnClickListener(new View.OnClickListener() { public void onClick(View v) {
                if (page != n) scroll.scrollTo(0, 0);  // a new page starts at its top; a rebuilt one keeps its place
                page = n;
                capturing = null;
                refresh();
            } });
            tabs.add(t);
            side.addView(t, new LinearLayout.LayoutParams(-1, -2));
        }
        // Update entry and its state line, pinned to the bottom of the sidebar (as on the PC launcher).
        side.addView(new View(this), new LinearLayout.LayoutParams(-1, 0, 1));
        final TextView upd = text("Update", 18, TEXT);
        upd.setTypeface(Typeface.SERIF, Typeface.BOLD);
        upd.setPadding(dp(12), dp(4), dp(12), dp(2));
        upd.setFocusable(true);
        upd.setClickable(true);
        upd.setOnClickListener(new View.OnClickListener() { public void onClick(View v) {
            if (page != 6) scroll.scrollTo(0, 0);
            page = 6;
            capturing = null;
            refresh();
        } });
        tabs.add(upd);
        side.addView(upd, new LinearLayout.LayoutParams(-1, -2));
        updState = text("", 12, DIM);
        updState.setPadding(dp(12), 0, dp(12), 0);
        updState.setSingleLine(true);
        updState.setEllipsize(android.text.TextUtils.TruncateAt.END);
        side.addView(updState);

        LinearLayout main = new LinearLayout(this);
        main.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams mlp = new LinearLayout.LayoutParams(0, -1, 1);
        mlp.leftMargin = dp(14);
        row.addView(main, mlp);

        scroll = new ScrollView(this);
        scroll.setBackground(box(PANEL, dp(8)));
        content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(18), dp(12), dp(18), dp(12));
        scroll.addView(content);
        main.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));

        LinearLayout bottom = new LinearLayout(this);
        bottom.setOrientation(LinearLayout.HORIZONTAL);
        bottom.setGravity(Gravity.CENTER_VERTICAL);
        bottom.setPadding(dp(8), dp(10), 0, 0);
        status = text("", 13, DIM);
        bottom.addView(status, new LinearLayout.LayoutParams(0, -2, 1));
        play = new Button(this);
        play.setText("PLAY");
        play.setTextSize(20);
        play.setTypeface(Typeface.SERIF, Typeface.BOLD);
        play.setAllCaps(false);
        play.setOnClickListener(new View.OnClickListener() { public void onClick(View v) { startGame(); } });
        bottom.addView(play, new LinearLayout.LayoutParams(dp(170), dp(52)));
        main.addView(bottom, new LinearLayout.LayoutParams(-1, -2));
        setContentView(frame);
    }

    TextView text(String s, float sp, int color) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        return t;
    }

    TextView heading(String s) {
        TextView t = text(s, 24, GOLD);
        t.setTypeface(Typeface.SERIF, Typeface.BOLD);
        t.setPadding(0, 0, 0, dp(4));
        content.addView(t);
        View rule = new View(this);
        rule.setBackgroundColor(0x99e2b15a);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, dp(1));
        lp.bottomMargin = dp(12);
        content.addView(rule, lp);
        return t;
    }

    TextView para(String s) {
        TextView t = text(s, 14, TEXT);
        t.setPadding(0, 0, 0, dp(8));
        content.addView(t);
        return t;
    }

    void checkLine(String kind, String s) {
        TextView t = text("", 14, TEXT);
        int c = kind.equals("ok") ? OK : kind.equals("missing") ? DIM : BAD;
        t.setText(android.text.Html.fromHtml("<font color='#" + Integer.toHexString(c & 0xffffff) + "'>[" + kind
                + "]</font>&nbsp;&nbsp;" + android.text.TextUtils.htmlEncode(s), 0));
        t.setPadding(0, dp(2), 0, dp(2));
        content.addView(t);
    }

    Button button(String label, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextColor(TEXT);
        b.setBackground(box(0xff4a3520, dp(6)));
        b.setPadding(dp(16), 0, dp(16), 0);
        b.setOnClickListener(l);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-2, dp(42));
        lp.topMargin = dp(8);
        content.addView(b, lp);
        return b;
    }

    void refresh() {
        boolean access = haveAccess();
        if (access) prepareFolders();
        if (access && migrating == null && !migrationDone) startMigration();
        dump = access ? scan() : null;
        showArt();
        for (int i = 0; i < tabs.size(); i++)
            tabs.get(i).setBackground(i == page ? box(SELECTED, dp(4)) : new ColorDrawable(Color.TRANSPARENT));
        content.removeAllViews();
        if (page == 6) pageUpdate();  // works without storage access too
        else if (!access) pageAccess();
        else if (page == 0) pageGame();
        else if (page == 1) pageSaves();
        else if (page == 2) pageControls();
        else if (page == 3) pageDisplay();
        else if (page == 4) pageOnline();
        else pageReport();
        showUpdState();
        boolean ready = dump != null && dump.ready();
        play.setEnabled(ready);
        play.setTextColor(ready ? 0xff2a1a08 : 0xff6b5d4b);
        play.setBackground(box(ready ? 0xffc99545 : 0xff3a3026, dp(6)));
        status.setText(!access ? "Storage access needed." : migrating != null ? migrating
                : ready && lastFailure != null ? "Last start failed: " + lastFailure
                : ready ? "Ready to play." : "Game files missing - see Game.");
        status.setTextColor(ready && lastFailure != null && migrating == null ? BAD : DIM);
        if (migrating != null) play.setEnabled(false);
    }

    // --- one-time move from the app's private folder ---------------------------------------------------------------
    // Builds before the launcher kept the dump in files/dump/ and saves + input.ini in files/vita_data/ (copied in with
    // adb run-as). Moved into OpenCrown/ once, without overwriting anything the user already put there.
    volatile String migrating;
    boolean migrationDone;

    void startMigration() {
        final File priv = getFilesDir();
        final File oldDump = new File(priv, "dump"), oldData = new File(priv, "vita_data");
        if (!oldDump.exists() && !oldData.exists()) { migrationDone = true; return; }
        migrating = "Moving files from the app's private folder...";
        new Thread(new Runnable() { public void run() {
            final long total = size(oldDump) + size(oldData);
            final long[] done = {0};
            Progress pr = new Progress() { public void add(long n) {
                done[0] += n;
                migrating = "Moving files from the app's private folder... " + (total > 0 ? done[0] * 100 / total : 100) + "%";
                runOnUiThread(new Runnable() { public void run() { status.setText(migrating); } });
            } };
            boolean ok = true;
            for (String part : new String[] {"Game", "Update"}) {
                File src = new File(oldDump, part), dst = new File(root, "dump/" + part);
                String[] have = dst.list();
                if (src.exists() && (have == null || have.length == 0)) ok &= copyTree(src, dst, pr);
            }
            File saves = new File(root, "saves");
            String[] haveSaves = saves.list();
            File oldSave = new File(oldData, "savedata0");
            if (oldSave.exists() && (haveSaves == null || haveSaves.length == 0)) ok &= copyTree(oldSave, saves, pr);
            File ini = new File(oldData, "input.ini"), newIni = new File(root, "input.ini");
            if (ini.exists() && !newIni.exists()) ok &= copyTree(ini, newIni, pr);
            if (ok) { delete(oldDump); delete(oldData); }
            final boolean fine = ok;
            runOnUiThread(new Runnable() { public void run() {
                migrating = null;
                migrationDone = true;
                if (!fine) status.setText("Some files could not be moved; the private copies were kept.");
                refresh();
            } });
        } }).start();
    }

    interface Progress { void add(long bytes); }

    static long size(File f) {
        if (f.isFile()) return f.length();
        long n = 0;
        File[] k = f.listFiles();
        if (k != null) for (File c : k) n += size(c);
        return n;
    }

    static boolean copyTree(File src, File dst, Progress pr) {
        if (src.isDirectory()) {
            dst.mkdirs();
            File[] k = src.listFiles();
            boolean ok = true;
            if (k != null) for (File c : k) ok &= copyTree(c, new File(dst, c.getName()), pr);
            return ok;
        }
        File tmp = new File(dst.getPath() + ".part");
        byte[] buf = new byte[1 << 20];
        try (java.io.FileInputStream in = new java.io.FileInputStream(src); FileOutputStream o = new FileOutputStream(tmp)) {
            for (int r; (r = in.read(buf)) > 0;) { o.write(buf, 0, r); pr.add(r); }
        } catch (IOException e) {
            tmp.delete();
            return false;
        }
        return tmp.length() == src.length() && tmp.renameTo(dst);
    }

    static void delete(File f) {
        File[] k = f.listFiles();
        if (k != null) for (File c : k) delete(c);
        f.delete();
    }

    // Box art and background from the user's own dump (sce_sys/icon0.png, pic0.png) once it is in place.
    void showArt() {
        Bitmap ic = null, bg = null;
        if (dump != null && dump.game != null) {
            ic = BitmapFactory.decodeFile(new File(dump.game.dir, "sce_sys/icon0.png").getPath());
            bg = BitmapFactory.decodeFile(new File(dump.game.dir, "sce_sys/pic0.png").getPath());
        }
        icon.setImageBitmap(ic);
        icon.setVisibility(ic != null ? View.VISIBLE : View.GONE);
        if (bg != null) {
            BitmapDrawable d = new BitmapDrawable(getResources(), bg);
            d.setGravity(Gravity.FILL);
            frame.setBackground(new LayerDrawable(new android.graphics.drawable.Drawable[] {d, new ColorDrawable(0x88000000)}));
        } else {
            frame.setBackgroundColor(BG);
        }
    }

    void pageAccess() {
        heading("Welcome");
        para("OpenCrown reads your game files and keeps your saves in one folder on this device:\n"
                + "Internal storage/OpenCrown/");
        para("Android needs to allow that first: tap below and switch on \"Allow access to manage all files\" "
                + "for OpenCrown Mobile, then come back.");
        button("Allow storage access", new View.OnClickListener() { public void onClick(View v) { requestAccess(); } }).requestFocus();
    }

    Button checked;
    String lastCheck;

    void pageGame() {
        heading("Game");
        para("Copy your own decrypted PS Vita dump of Dragon's Crown (US, " + TITLE_ID + ") into\n"
                + "Internal storage/OpenCrown/dump/  -  the base game in Game/, the 1.09 update in Update/.");
        for (String[] l : dump.lines) checkLine(l[0], l[1]);
        checked = button("Check again", new View.OnClickListener() { public void onClick(View v) {
            lastCheck = new SimpleDateFormat("HH:mm:ss", Locale.US).format(new Date());
            refresh();
            if (checked != null) checked.requestFocus();  // the rebuilt button keeps the focus (pad users)
        } });
        if (lastCheck != null) para("Checked at " + lastCheck + ".").setTextColor(DIM);
    }

    // --- saves -----------------------------------------------------------------------------------------------------
    static final int IMPORT_SAVE = 10, EXPORT_SAVE = 11;
    String saveMessage;

    File saveDir() { return new File(root, "saves"); }

    void pageSaves() {
        heading("Save Folder");
        para("Save folder (the game keeps SAVE0.DAT inside it): Internal storage/OpenCrown/saves/");
        File f = new File(saveDir(), "SAVE0.DAT");
        SaveFile s = SaveFile.inspect(f);
        if (s == null) checkLine("missing", "No save data yet (the game makes one when you start).");
        else if (s.problem != null) checkLine("error", "SAVE0.DAT " + s.problem);
        else checkLine("ok", "Save data present, last saved "
                + new SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.US).format(new Date(f.lastModified())));
        para("");
        para("Import a save from OpenCrown on PC (Documents\\Dragons Crown Pro\\saves\\SAVEDATA00\\SAVE0.DAT) "
                + "or from a PS Vita. Your current save is backed up to saves/backup/ first.");
        button("Import PC save...", new View.OnClickListener() {
            public void onClick(View v) {
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                i.addCategory(Intent.CATEGORY_OPENABLE);
                i.setType("*/*");
                startActivityForResult(i, IMPORT_SAVE);
            }
        });
        para("");
        para("Export your save to take it to OpenCrown on PC: pick where it goes (Downloads, a USB stick, Google Drive "
                + "...), then put it in Documents\\Dragons Crown Pro\\saves\\SAVEDATA00\\ on the PC. "
                + "No conversion needed.");
        Button export = button("Export save...", new View.OnClickListener() {
            public void onClick(View v) {
                Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);  // the system "save to" picker
                i.addCategory(Intent.CATEGORY_OPENABLE);
                i.setType("application/octet-stream");
                i.putExtra(Intent.EXTRA_TITLE, "SAVE0.DAT");
                startActivityForResult(i, EXPORT_SAVE);
            }
        });
        export.setEnabled(s != null && s.problem == null);
        if (saveMessage != null) {
            TextView t = para(saveMessage);
            t.setTextColor(saveMessage.startsWith("Imported") || saveMessage.startsWith("Exported") ? OK : BAD);
        }
    }

    @Override
    protected void onActivityResult(int code, int result, Intent data) {
        super.onActivityResult(code, result, data);
        if ((code != IMPORT_SAVE && code != EXPORT_SAVE) || result != RESULT_OK || data == null || data.getData() == null)
            return;
        saveMessage = code == IMPORT_SAVE ? importSave(data.getData()) : exportSave(data.getData());
        page = 1;
        refresh();
    }

    // Copies saves/SAVE0.DAT as it is to the place picked in the system dialog (same format as PC / PS4 / PS5).
    String exportSave(Uri uri) {
        File cur = new File(saveDir(), "SAVE0.DAT");
        byte[] b;
        try (java.io.FileInputStream in = new java.io.FileInputStream(cur)) {
            b = SaveFile.readAll(in, SaveFile.SIZE + 1);
        } catch (IOException e) {
            return "Could not read your save: " + e.getMessage();
        }
        SaveFile s = SaveFile.parse(b);
        if (s.problem != null) return "Your save could not be exported: " + s.problem + ".";
        try (java.io.OutputStream o = getContentResolver().openOutputStream(uri, "wt")) {
            if (o == null) return "Export failed: that place cannot be written.";
            o.write(b);
        } catch (IOException | SecurityException e) {
            return "Export failed: " + e.getMessage();
        }
        return "Exported SAVE0.DAT. On the PC, put it in Documents\\Dragons Crown Pro\\saves\\SAVEDATA00\\ "
                + "(keep a copy of the PC's own save first).";
    }

    String importSave(Uri uri) {
        byte[] b;
        try (InputStream in = getContentResolver().openInputStream(uri)) {
            b = SaveFile.readAll(in, SaveFile.SIZE + 1);
        } catch (IOException | SecurityException e) {
            return "Could not read that file: " + e.getMessage();
        }
        SaveFile s = SaveFile.parse(b);
        if (s.problem != null) return "That is not a Dragon's Crown save: " + s.problem + ".";
        if (!s.supported())
            return "Unknown save version 0x" + Integer.toHexString(s.version) + ".";
        return install(b);
    }

    // Backs up the current save, writes the new one and makes sure the game sees slot 0.
    String install(byte[] b) {
        File dir = saveDir(), cur = new File(dir, "SAVE0.DAT");
        try {
            if (cur.exists()) {
                File bak = new File(dir, "backup");
                bak.mkdirs();
                String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date());
                if (!cur.renameTo(new File(bak, "SAVE0-" + stamp + ".DAT"))) return "Could not back up the current save.";
            }
            try (FileOutputStream o = new FileOutputStream(cur)) {
                o.write(b);
            }
            File slot = new File(dir, "sce_sys/oc_slot_0.bin");
            if (!slot.exists()) {  // our SceAppUtil slot record (status 0 = usable); the game fills it on its next save
                slot.getParentFile().mkdirs();
                try (FileOutputStream o = new FileOutputStream(slot)) {
                    o.write(new byte[SaveFile.SLOT_PARAM_SIZE]);
                }
            }
        } catch (IOException e) {
            return "Import failed: " + e.getMessage();
        }
        return "Imported. Start the game and pick Start Game.";
    }

    // --- controls: controller buttons in OpenCrown/input.ini (pad_* keys, SDL gamepad button names, read by
    // src/platform/sdl_app.cpp). Keyboard bindings live in the same file and keep working; this page shows the pad.
    // Layout follows the PC launcher's Controls page (Action | Binding table, Reset to defaults / Save / Revert).
    static final String[][] PAD_ACTIONS = {  // {input.ini key, label, default}
        {"pad_cross", "Jump (Cross)", "a"},
        {"pad_square", "Attack (Square)", "x"},
        {"pad_circle", "Item / slide (Circle)", "b"},
        {"pad_triangle", "Triangle", "y"},
        {"pad_l", "L", "leftshoulder"},
        {"pad_r", "R", "rightshoulder"},
        {"pad_start", "Start / menu", "start"},
        {"pad_select", "Select (town map)", "back"},
        {"pad_dpad_up", "D-pad up", "dpup"},
        {"pad_dpad_down", "D-pad down", "dpdown"},
        {"pad_dpad_left", "D-pad left", "dpleft"},
        {"pad_dpad_right", "D-pad right", "dpright"},
    };

    final java.util.Map<String, String> padBinds = new java.util.HashMap<>();
    boolean padLoaded;
    String capturing;      // input.ini key waiting for a button press
    String controlsStatus = "";

    static String padLabel(String sdl) {
        switch (sdl) {
            case "": return "(unbound)";
            case "a": return "Bottom button (A)";
            case "b": return "Right button (B)";
            case "x": return "Left button (X)";
            case "y": return "Top button (Y)";
            case "leftshoulder": return "L1";
            case "rightshoulder": return "R1";
            case "lefttrigger": return "L2";
            case "righttrigger": return "R2";
            case "leftstick": return "L3 (left stick press)";
            case "rightstick": return "R3 (right stick press)";
            case "start": return "Start";
            case "back": return "Select / Back";
            case "dpup": return "D-pad up";
            case "dpdown": return "D-pad down";
            case "dpleft": return "D-pad left";
            case "dpright": return "D-pad right";
        }
        return sdl;
    }

    // Android key codes of controller buttons -> SDL gamepad button names (positional, like SDL's mapping).
    static String padName(int keyCode) {
        switch (keyCode) {
            case android.view.KeyEvent.KEYCODE_BUTTON_A: return "a";
            case android.view.KeyEvent.KEYCODE_BUTTON_B: return "b";
            case android.view.KeyEvent.KEYCODE_BUTTON_X: return "x";
            case android.view.KeyEvent.KEYCODE_BUTTON_Y: return "y";
            case android.view.KeyEvent.KEYCODE_BUTTON_L1: return "leftshoulder";
            case android.view.KeyEvent.KEYCODE_BUTTON_R1: return "rightshoulder";
            case android.view.KeyEvent.KEYCODE_BUTTON_L2: return "lefttrigger";
            case android.view.KeyEvent.KEYCODE_BUTTON_R2: return "righttrigger";
            case android.view.KeyEvent.KEYCODE_BUTTON_THUMBL: return "leftstick";
            case android.view.KeyEvent.KEYCODE_BUTTON_THUMBR: return "rightstick";
            case android.view.KeyEvent.KEYCODE_BUTTON_START: return "start";
            case android.view.KeyEvent.KEYCODE_BUTTON_SELECT: return "back";
            case android.view.KeyEvent.KEYCODE_DPAD_UP: return "dpup";
            case android.view.KeyEvent.KEYCODE_DPAD_DOWN: return "dpdown";
            case android.view.KeyEvent.KEYCODE_DPAD_LEFT: return "dpleft";
            case android.view.KeyEvent.KEYCODE_DPAD_RIGHT: return "dpright";
        }
        return null;
    }

    Ini inputIni() { return new Ini(new File(root, "input.ini")); }

    void loadPad() {
        Ini ini = inputIni();
        for (String[] a : PAD_ACTIONS) {
            String v = ini.get(a[0]);
            padBinds.put(a[0], v == null ? a[2] : v.trim());
        }
        padLoaded = true;
    }

    // While a binding waits for a button, the next controller button press is the new binding (it does not move the
    // focus); Back / Escape cancels.
    @Override
    public boolean dispatchKeyEvent(android.view.KeyEvent e) {
        if (capturing != null && page == 2) {
            int k = e.getKeyCode();
            if (k == android.view.KeyEvent.KEYCODE_BACK || k == android.view.KeyEvent.KEYCODE_ESCAPE) {
                if (e.getAction() == android.view.KeyEvent.ACTION_UP) {
                    capturing = null;
                    controlsStatus = "";
                    refresh();
                }
                return true;
            }
            String n = padName(k);
            if (n != null) {
                if (e.getAction() == android.view.KeyEvent.ACTION_UP) {
                    padBinds.put(capturing, n);
                    capturing = null;
                    controlsStatus = "Not saved yet.";
                    refresh();
                }
                return true;
            }
        }
        return super.dispatchKeyEvent(e);
    }

    void pageControls() {
        heading("Controls");
        if (!padLoaded) loadPad();
        para("Tap a binding, then press a button on your controller. Back cancels.").setTextColor(DIM);
        LinearLayout hr = row();
        hr.addView(text("Action", 14, GOLD), new LinearLayout.LayoutParams(dp(220), -2));
        hr.addView(text("Controller button", 14, GOLD), new LinearLayout.LayoutParams(0, -2, 1));
        java.util.Map<String, Integer> uses = new java.util.HashMap<>();
        for (String[] a : PAD_ACTIONS) {
            String v = padBinds.get(a[0]);
            if (!v.isEmpty()) uses.put(v, uses.containsKey(v) ? uses.get(v) + 1 : 1);
        }
        for (final String[] a : PAD_ACTIONS) {
            LinearLayout r = row();
            r.addView(text(a[1], 15, TEXT), new LinearLayout.LayoutParams(dp(220), -2));
            String v = padBinds.get(a[0]);
            boolean cap = a[0].equals(capturing);
            Button b = smallButton(r, cap ? "Press a controller button..." : padLabel(v), new View.OnClickListener() {
                public void onClick(View view) {
                    capturing = a[0];
                    controlsStatus = "";
                    refresh();
                }
            });
            ((LinearLayout.LayoutParams) b.getLayoutParams()).leftMargin = 0;
            b.setLayoutParams(new LinearLayout.LayoutParams(0, dp(36), 1));
            if (v.isEmpty()) b.setTextColor(DIM);
            else if (uses.get(v) > 1) b.setTextColor(WARN);  // same button on two actions
            if (cap) b.requestFocus();
        }
        para("Sticks are fixed: left stick moves, right stick moves the pointer. Touching the screen is the game's "
                + "touch screen (fairy pointer).").setTextColor(DIM);
        LinearLayout br = row();
        Button reset = smallButton(br, "Reset to defaults", new View.OnClickListener() { public void onClick(View v) {
            for (String[] a : PAD_ACTIONS) padBinds.put(a[0], a[2]);
            capturing = null;
            controlsStatus = "Defaults restored (not saved yet).";
            refresh();
        } });
        ((LinearLayout.LayoutParams) reset.getLayoutParams()).leftMargin = 0;
        smallButton(br, "Save", new View.OnClickListener() { public void onClick(View v) {
            Ini ini = inputIni();
            for (String[] a : PAD_ACTIONS) ini.set(a[0], padBinds.get(a[0]));
            capturing = null;
            controlsStatus = "Saved.";
            refresh();
        } });
        smallButton(br, "Revert", new View.OnClickListener() { public void onClick(View v) {
            loadPad();
            capturing = null;
            controlsStatus = "Reloaded from input.ini.";
            refresh();
        } });
        span(br, controlsStatus, DIM);

        sub("Touch controls");
        final TextView label = para("");
        final Ini ini = inputIni();
        float cur = 0.6f;
        try {
            String v = ini.get("touch_opacity");
            if (v != null) cur = Float.parseFloat(v);
        } catch (NumberFormatException ignored) {
        }
        SeekBar bar = new SeekBar(this);
        bar.setMax(100);
        bar.setProgress(Math.round(cur * 100));
        label.setText("On-screen controls opacity: " + bar.getProgress() + "%");
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar s, int p, boolean user) {
                label.setText("On-screen controls opacity: " + p + "%" + (p == 0 ? " (hidden)" : ""));
            }
            public void onStartTrackingTouch(SeekBar s) {}
            public void onStopTrackingTouch(SeekBar s) {
                ini.set("touch_opacity", String.format(Locale.US, "%.2f", s.getProgress() / 100f));
            }
        });
        content.addView(bar, new LinearLayout.LayoutParams(-1, dp(40)));
        para("Shown while no controller is connected. Keyboards also work (bindings in input.ini).").setTextColor(DIM);
    }

    // --- display: OpenCrown/display.ini, read by src/platform/sdl_app.cpp at game start ---------------------------
    void pageDisplay() {
        heading("Display");
        final Ini ini = new Ini(new File(root, "display.ini"));
        para("Renderer");
        RadioGroup g = new RadioGroup(this);
        RadioButton vk = new RadioButton(this), gl = new RadioButton(this);
        vk.setText("Vulkan");
        gl.setText("OpenGL ES (not available yet)");
        for (RadioButton b : new RadioButton[] {vk, gl}) {
            b.setTextColor(TEXT);
            b.setId(View.generateViewId());
            g.addView(b);
        }
        gl.setEnabled(false);
        gl.setTextColor(DIM);
        vk.setChecked(true);
        content.addView(g);
        if (ini.get("renderer") == null) ini.set("renderer", "vulkan");
    }

    // --- online: OpenCrown/online.ini (keys as the PC launcher's launcher.ini: lobby, name, upnp), passed to the game as
    // --lobby / --online-name / --no-upnp. Page order and wording follow the PC launcher's Online page.
    static final String DEFAULT_LOBBY = "wss://opencrown-lobby.networkhandler.workers.dev/";

    Ini onlineIni() { return new Ini(new File(root, "online.ini")); }

    String lobbyUrl() {
        String v = onlineIni().get("lobby");
        return v == null || v.isEmpty() ? DEFAULT_LOBBY : v;
    }

    boolean upnpOn() {
        String v = onlineIni().get("upnp");
        return v == null || !(v.equals("0") || v.equalsIgnoreCase("false"));
    }

    // The game's online-id rule (src/net/identity.cpp online_name): 3-16 of A-Z a-z 0-9 _ -.
    static boolean validName(String s) {
        if (s == null || s.length() < 3 || s.length() > 16) return false;
        for (int i = 0; i < s.length(); i++) if (!nameChar(s.charAt(i))) return false;
        return true;
    }

    static boolean nameChar(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    }

    String startName;  // the valid name at launcher start: a field left invalid goes back to it (as on PC)

    // First start (or a broken name): a random PlayerNNNN, saved silently, as the PC launcher does.
    String playerName() {
        Ini ini = onlineIni();
        String n = ini.get("name");
        if (!validName(n)) {
            n = "Player" + (1000 + new java.util.Random().nextInt(9000));
            ini.set("name", n);
        }
        if (startName == null) startName = n;
        return n;
    }

    EditText field(String value, String hint) {
        EditText e = new EditText(this);
        e.setText(value);
        e.setHint(hint);
        e.setSingleLine(true);
        e.setTextColor(TEXT);
        e.setHintTextColor(DIM);
        e.setTextSize(16);
        e.setImeOptions(EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_EXTRACT_UI);
        return e;
    }

    TextView sub(String s) {  // a section heading inside a page
        TextView t = text(s, 18, GOLD);
        t.setTypeface(Typeface.SERIF, Typeface.BOLD);
        t.setPadding(0, dp(12), 0, dp(4));
        content.addView(t);
        return t;
    }

    void pageOnline() {
        heading("Online");
        final Ini ini = onlineIni();

        sub("Player name");
        LinearLayout nr = row();
        final EditText nameField = field(playerName(), "Player1234");
        nameField.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
                | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD);  // no autocorrect / suggestions
        nameField.setFilters(new android.text.InputFilter[] {
            new android.text.InputFilter.LengthFilter(16),
            new android.text.InputFilter() {
                public CharSequence filter(CharSequence src, int start, int end, android.text.Spanned d, int ds, int de) {
                    StringBuilder ok = new StringBuilder();
                    for (int i = start; i < end; i++) if (nameChar(src.charAt(i))) ok.append(src.charAt(i));
                    return ok.length() == end - start ? null : ok;
                }
            }});
        nameField.addTextChangedListener(new TextWatcher() {
            public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            public void onTextChanged(CharSequence s, int a, int b, int c) {}
            public void afterTextChanged(Editable s) { if (validName(s.toString())) ini.set("name", s.toString()); }
        });
        nameField.setOnFocusChangeListener(new View.OnFocusChangeListener() {
            public void onFocusChange(View v, boolean focus) {
                if (!focus && !validName(nameField.getText().toString())) {
                    nameField.setText(startName);
                    ini.set("name", startName);
                }
            }
        });
        nr.addView(nameField, new LinearLayout.LayoutParams(dp(260), -2));
        TextView hint = text("3-16 letters, digits, _ or -", 13, DIM);
        hint.setPadding(dp(10), 0, 0, 0);
        nr.addView(hint);

        sub("Lobby server");
        para("Introductions only; game traffic goes directly between players.").setTextColor(DIM);
        LinearLayout lr = row();
        String lobby = ini.get("lobby");
        final EditText lobbyField = field(lobby == null || lobby.isEmpty() ? DEFAULT_LOBBY : lobby, DEFAULT_LOBBY);
        lobbyField.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        lobbyField.addTextChangedListener(new TextWatcher() {
            public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            public void onTextChanged(CharSequence s, int a, int b, int c) {}
            public void afterTextChanged(Editable s) { ini.set("lobby", s.toString().trim()); }
        });
        lr.addView(lobbyField, new LinearLayout.LayoutParams(0, -2, 1));
        smallButton(lr, "Default", new View.OnClickListener() { public void onClick(View v) {
            lobbyField.setText(DEFAULT_LOBBY);
            friendsDoneMs = 0;  // another server: other friends
            runFriends(Net.NONE, null);
        } });
        android.widget.CheckBox up = new android.widget.CheckBox(this);
        up.setText("Open the game port on the router (UPnP)");
        up.setTextColor(TEXT);
        up.setChecked(upnpOn());
        up.setOnCheckedChangeListener(new android.widget.CompoundButton.OnCheckedChangeListener() {
            public void onCheckedChanged(android.widget.CompoundButton b, boolean on) { ini.set("upnp", on ? "true" : "false"); }
        });
        content.addView(up);
        checkBox = new LinearLayout(this);
        checkBox.setOrientation(LinearLayout.VERTICAL);
        content.addView(checkBox, new LinearLayout.LayoutParams(-1, -2));
        showCheck();
        para("Go online from the game's own online menu; these settings apply when it connects.").setTextColor(DIM);

        sub("Friends");
        friendsBox = new LinearLayout(this);
        friendsBox.setOrientation(LinearLayout.VERTICAL);
        content.addView(friendsBox, new LinearLayout.LayoutParams(-1, -2));
        showFriends();
        if (friendsDoneMs == 0 || System.currentTimeMillis() - friendsDoneMs > 30000) runFriends(Net.NONE, null);
        scheduleFriendsRefresh();
    }

    // Runs `fill` with the page helpers (para, button, ...) adding to `box` instead of the page: a section rebuilt
    // alone when a background job finishes (the text fields being typed in stay as they are).
    void fillBox(LinearLayout box, Runnable fill) {
        LinearLayout page = content;
        box.removeAllViews();
        content = box;
        try { fill.run(); } finally { content = page; }
    }

    LinearLayout row() {
        LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        content.addView(r, new LinearLayout.LayoutParams(-1, -2));
        return r;
    }

    Button smallButton(LinearLayout r, String label, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextColor(TEXT);
        b.setTextSize(13);
        b.setBackground(box(0xff4a3520, dp(6)));
        b.setPadding(dp(12), 0, dp(12), 0);
        b.setOnClickListener(l);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-2, dp(36));
        lp.leftMargin = dp(8);
        r.addView(b, lp);
        return b;
    }

    TextView span(LinearLayout r, String s, int color) {
        TextView t = text(s, 14, color);
        t.setPadding(dp(8), 0, 0, 0);
        r.addView(t);
        return t;
    }

    // "Label: value" with the label coloured (green / red / orange) and the value in body text.
    void labelLine(String label, int color, String value) {
        TextView t = text("", 14, TEXT);
        t.setText(android.text.Html.fromHtml("<font color='#" + Integer.toHexString(color & 0xffffff) + "'>"
                + android.text.TextUtils.htmlEncode(label) + "</font> " + android.text.TextUtils.htmlEncode(value), 0));
        t.setPadding(0, dp(2), 0, dp(2));
        content.addView(t);
    }

    // --- Test connection (net/online_check: lobby + hello, STUN, UPnP probe) ---
    static final int WARN = 0xfffa9e4d;
    LinearLayout checkBox;
    boolean checkBusy;
    Net.Check checkResult;
    String checkError;

    void showCheck() {
        if (checkBox == null || page != 4) return;
        fillBox(checkBox, new Runnable() { public void run() {
            LinearLayout r = row();
            Button b = smallButton(r, checkBusy ? "Testing..." : "Test connection", new View.OnClickListener() {
                public void onClick(View v) { runCheck(); }
            });
            ((LinearLayout.LayoutParams) b.getLayoutParams()).leftMargin = 0;
            b.setEnabled(!checkBusy);
            if (checkResult != null) {
                Net.Check c = checkResult;
                span(r, c.verdict, c.lobbyOk && c.stunOk && c.natSame != 0 ? OK : c.lobbyOk ? WARN : BAD).setTextSize(13);
                labelLine("Lobby:", c.lobbyOk ? OK : BAD, c.lobby);
                labelLine("Public UDP endpoint:", c.stunOk ? OK : BAD, c.endpoint);
                if (c.upnpTried) labelLine("UPnP:", c.upnpOk ? OK : WARN, c.upnp);
            } else if (checkError != null) {
                span(r, checkError, BAD);
            }
        } });
    }

    void runCheck() {
        if (checkBusy) return;
        checkBusy = true;
        showCheck();
        final String lobby = lobbyUrl(), name = playerName();
        final boolean upnp = upnpOn();
        new Thread(new Runnable() { public void run() {
            Net.Check r = null;
            String err = null;
            try {
                r = Net.check(lobby, name, upnp);
            } catch (Throwable t) {
                err = "Test failed: " + t;
            }
            final Net.Check res = r;
            final String e = err;
            runOnUiThread(new Runnable() { public void run() {
                checkBusy = false;
                checkResult = res;
                checkError = e;
                showCheck();
            } });
        } }).start();
    }

    // --- friends (net/friends: one short lobby session per refresh / action, every 30 s while the page is open) ---
    LinearLayout friendsBox;
    boolean friendsBusy;
    long friendsDoneMs;
    Net.Friends friendsState;
    String friendsMessage, confirmRemove, addCode = "";
    boolean friendsMessageOk;
    final android.os.Handler handler = new android.os.Handler(android.os.Looper.getMainLooper());
    final Runnable friendsTick = new Runnable() { public void run() {
        if (page != 4 || isFinishing()) return;
        runFriends(Net.NONE, null);
        scheduleFriendsRefresh();
    } };

    void scheduleFriendsRefresh() {
        handler.removeCallbacks(friendsTick);
        handler.postDelayed(friendsTick, 30000);
    }

    void runFriends(final int action, final String arg) {
        if (friendsBusy) return;
        friendsBusy = true;
        showFriends();
        final String lobby = lobbyUrl(), name = playerName(), dir = root.getAbsolutePath();
        new Thread(new Runnable() { public void run() {
            Net.Friends r;
            try {
                r = Net.session(dir, lobby, name, action, arg);
            } catch (Throwable t) {
                r = new Net.Friends();
                r.error = "friends failed: " + t;
            }
            final Net.Friends res = r;
            runOnUiThread(new Runnable() { public void run() {
                friendsBusy = false;
                friendsDoneMs = System.currentTimeMillis();
                friendsState = res;
                if (res.acted) {
                    friendsMessage = res.actionMsg;
                    friendsMessageOk = res.actionOk;
                }
                showFriends();
            } });
        } }).start();
    }

    void showFriends() {
        if (friendsBox == null || page != 4) return;
        fillBox(friendsBox, new Runnable() { public void run() { fillFriends(); } });
    }

    void fillFriends() {
        final Net.Friends f = friendsState;
        if (f != null && !f.identityOk) {
            para("No online identity: " + f.identityError).setTextColor(BAD);
            return;
        }
        LinearLayout r = row();
        r.addView(text("Your friend code:", 15, TEXT));
        if (f != null && !f.code.isEmpty()) {
            span(r, f.code, 0xfff2b359).setTextSize(16);
            smallButton(r, "Copy", new View.OnClickListener() { public void onClick(View v) {
                android.content.ClipboardManager cm = (android.content.ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
                if (cm != null) cm.setPrimaryClip(android.content.ClipData.newPlainText("friend code", f.code));
            } });
        } else {
            span(r, friendsBusy ? "asking the lobby..." : "-", DIM);
        }
        smallButton(r, friendsBusy ? "Updating..." : "Refresh", new View.OnClickListener() {
            public void onClick(View v) { runFriends(Net.NONE, null); }
        }).setEnabled(!friendsBusy);
        if (f != null && !f.ok && !f.error.isEmpty()) para(f.error).setTextColor(BAD);

        LinearLayout add = row();
        add.addView(text("Add a friend by code:", 15, TEXT));
        final EditText codeField = field(addCode, "XXXX-XXXX");
        codeField.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS
                | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD);
        codeField.setFilters(new android.text.InputFilter[] {new android.text.InputFilter.AllCaps(),
                new android.text.InputFilter.LengthFilter(11)});
        codeField.addTextChangedListener(new TextWatcher() {
            public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            public void onTextChanged(CharSequence s, int a, int b, int c) {}
            public void afterTextChanged(Editable s) { addCode = s.toString(); }
        });
        LinearLayout.LayoutParams clp = new LinearLayout.LayoutParams(dp(150), -2);
        clp.leftMargin = dp(8);
        add.addView(codeField, clp);
        smallButton(add, "Paste", new View.OnClickListener() { public void onClick(View v) {
            android.content.ClipboardManager cm = (android.content.ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
            if (cm != null && cm.hasPrimaryClip() && cm.getPrimaryClip().getItemCount() > 0) {
                CharSequence t = cm.getPrimaryClip().getItemAt(0).coerceToText(LauncherActivity.this);
                if (t != null) codeField.setText(t.toString().trim());
            }
        } });
        smallButton(add, "Send request", new View.OnClickListener() { public void onClick(View v) {
            String c = addCode.trim();
            if (c.isEmpty()) return;
            addCode = "";
            runFriends(Net.REQUEST, c);
        } }).setEnabled(!friendsBusy && f != null && f.ok);
        if (friendsMessage != null) span(add, friendsMessage, friendsMessageOk ? OK : BAD);
        if (f == null || !f.ok) return;

        if (!f.incoming.isEmpty()) {
            para("Friend requests (" + f.incoming.size() + "):").setTextColor(WARN);
            for (final Net.Person p : f.incoming) {
                LinearLayout pr = row();
                pr.addView(text("  • " + (p.name.isEmpty() ? "(no name)" : p.name), 15, TEXT));
                smallButton(pr, "Accept", new View.OnClickListener() { public void onClick(View v) {
                    runFriends(Net.ACCEPT, p.id);
                } }).setEnabled(!friendsBusy);
                smallButton(pr, "Decline", new View.OnClickListener() { public void onClick(View v) {
                    runFriends(Net.DECLINE, p.id);
                } }).setEnabled(!friendsBusy);
            }
        }
        if (!f.outgoing.isEmpty()) {
            para("Sent requests, waiting for an answer:").setTextColor(DIM);
            for (final Net.Person p : f.outgoing) {
                LinearLayout pr = row();
                pr.addView(text("  • " + (p.name.isEmpty() ? "(no name)" : p.name), 15, TEXT));
                smallButton(pr, "Cancel", new View.OnClickListener() { public void onClick(View v) {
                    runFriends(Net.DECLINE, p.id);
                } }).setEnabled(!friendsBusy);
            }
        }
        if (f.friends.isEmpty()) {
            para("No friends yet: send your code to a friend, or add theirs above.").setTextColor(DIM);
            return;
        }
        para("Friends (" + f.online() + " online of " + f.friends.size() + "):");
        for (final Net.Person p : f.friends) {
            LinearLayout pr = row();
            pr.addView(text(p.name, 15, TEXT), new LinearLayout.LayoutParams(dp(200), -2));
            String status = !p.online ? "Offline" : p.joinCode.isEmpty() ? "Online" : "Online, in a room";
            pr.addView(text(status, 15, p.online ? OK : DIM), new LinearLayout.LayoutParams(0, -2, 1));
            final boolean confirm = p.id.equals(confirmRemove);
            smallButton(pr, confirm ? "Really remove?" : "Remove", new View.OnClickListener() {
                public void onClick(View v) {
                    if (confirm) {
                        confirmRemove = null;
                        runFriends(Net.REMOVE, p.id);
                    } else {
                        confirmRemove = p.id;
                        showFriends();
                    }
                }
            }).setEnabled(!friendsBusy);
        }
    }

    // --- Report a Bug (net/bug_report: the note + the redacted tail of the last game log, through the lobby) -------
    static final int BUG_TEXT_MAX = 256;  // net/bug_report.h kBugReportTextMax
    String bugNote = "", bugMessage;
    boolean bugBusy, bugOk;
    int bugLeft = -1;

    File logFile() { return new File(root, "log.txt"); }

    String appVersion() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "?";
        }
    }

    void pageReport() {
        heading("Report a Bug");
        para("Describe what went wrong in a few words. The note is sent together with the log of your last game "
                + "session, so report right after the problem happens.");
        final EditText note = new EditText(this);
        note.setText(bugNote);
        note.setTextColor(TEXT);
        note.setHintTextColor(DIM);
        note.setTextSize(15);
        note.setGravity(Gravity.TOP | Gravity.START);
        note.setMinLines(4);
        note.setMaxLines(6);
        note.setFilters(new android.text.InputFilter[] {new android.text.InputFilter.LengthFilter(BUG_TEXT_MAX)});
        note.setImeOptions(EditorInfo.IME_FLAG_NO_EXTRACT_UI);
        content.addView(note, new LinearLayout.LayoutParams(-1, -2));
        final TextView count = para(bugNote.length() + " / " + BUG_TEXT_MAX);
        count.setTextColor(DIM);
        final LinearLayout r = row();
        final Button send = smallButton(r, bugBusy ? "Sending..." : "Send report", new View.OnClickListener() {
            public void onClick(View v) { sendReport(); }
        });
        ((LinearLayout.LayoutParams) send.getLayoutParams()).leftMargin = 0;
        send.setEnabled(!bugBusy && !bugNote.trim().isEmpty());
        smallButton(r, "Preview", new View.OnClickListener() { public void onClick(View v) { previewReport(); } });
        if (bugMessage != null) span(r, bugMessage, bugOk ? OK : BAD);
        note.addTextChangedListener(new TextWatcher() {
            public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            public void onTextChanged(CharSequence s, int a, int b, int c) {}
            public void afterTextChanged(Editable s) {
                bugNote = s.toString();
                count.setText(bugNote.length() + " / " + BUG_TEXT_MAX);
                send.setEnabled(!bugBusy && !bugNote.trim().isEmpty());
            }
        });
        if (bugLeft >= 0) para("Reports left today: " + bugLeft).setTextColor(DIM);
        boolean haveLog = logFile().isFile();
        para("Sent: your note, player name and online id, your Android version, and the last 128 KB of the game log"
                + (haveLog ? "" : " (no log yet: play once first)") + ". IP addresses, other players' ids and your "
                + "device's user name are removed first; Preview shows exactly what is sent. Up to 5 reports a day.")
                .setTextColor(DIM);
    }

    void previewReport() {
        final String dir = root.getAbsolutePath(), log = logFile().getAbsolutePath(), text = bugNote;
        new Thread(new Runnable() { public void run() {
            String l;
            try { l = Net.bugLog(dir, log); } catch (Throwable t) { l = "(could not read the log: " + t + ")"; }
            final String body = "Note:\n" + text + "\n\nLog (" + log + ", redacted):\n" + (l.isEmpty() ? "(none)" : l);
            runOnUiThread(new Runnable() { public void run() {
                TextView t = text(body, 12, TEXT);
                t.setTextIsSelectable(true);
                t.setPadding(dp(16), dp(8), dp(16), dp(8));
                ScrollView s = new ScrollView(LauncherActivity.this);
                s.addView(t);
                new android.app.AlertDialog.Builder(LauncherActivity.this)
                        .setTitle("Report preview").setView(s).setPositiveButton("Close", null).show();
            } });
        } }).start();
    }

    void sendReport() {
        if (bugBusy || bugNote.trim().isEmpty()) return;
        bugBusy = true;
        refresh();
        final String dir = root.getAbsolutePath(), lobby = lobbyUrl(), name = playerName(), text = bugNote.trim(),
                log = logFile().getAbsolutePath(), version = "mobile launcher " + appVersion();
        new Thread(new Runnable() { public void run() {
            Net.Report r;
            try {
                r = Net.report(dir, lobby, name, text, log, version);
            } catch (Throwable t) {
                r = new Net.Report();
                r.msg = "Report failed: " + t;
            }
            final Net.Report res = r;
            runOnUiThread(new Runnable() { public void run() {
                bugBusy = false;
                bugOk = res.ok;
                bugMessage = res.msg;
                if (res.left >= 0) bugLeft = res.left;
                if (res.ok) bugNote = "";
                if (page == 5) refresh();
            } });
        } }).start();
    }

    // --- Update (Update.java: GitHub releases -> newer OpenCrownMobile APK, checked, installed by Android) --------
    // OpenCrown/launcher.ini check_updates = the PC launcher's key: the start-up check, on unless set to false.
    TextView updState;
    boolean updBusy, updChecked, updDownloading, updCommitted;
    long updGot, updTotal;
    Update.Result updResult;
    String updError, updatedFrom;
    final java.util.concurrent.atomic.AtomicBoolean updCancel = new java.util.concurrent.atomic.AtomicBoolean();
    android.content.BroadcastReceiver updReceiver;

    File updMarker() { return new File(getFilesDir(), "update_from.txt"); }  // the version an install started from

    boolean checkUpdatesOn() {
        String v = new Ini(new File(root, "launcher.ini")).get("check_updates");
        return v == null || !(v.equals("0") || v.equalsIgnoreCase("false"));
    }

    void startUpdates() {
        updReceiver = new android.content.BroadcastReceiver() {
            public void onReceive(android.content.Context c, Intent i) { installStatus(i); }
        };
        android.content.IntentFilter f = new android.content.IntentFilter(Update.ACTION_STATUS);
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(updReceiver, f, RECEIVER_NOT_EXPORTED);
        else registerReceiver(updReceiver, f);
        File m = updMarker();
        if (m.isFile()) {
            String from = "";
            try (java.io.FileInputStream in = new java.io.FileInputStream(m)) {
                from = new String(SaveFile.readAll(in, 256), "UTF-8").trim();
            } catch (IOException ignored) {
            }
            m.delete();
            int[] was = Update.parseVersion(from), now = Update.parseVersion(appVersion());
            if (was != null && now != null && Update.compare(now, was) > 0) {
                updatedFrom = from;
                page = 6;
                refresh();
            }
        }
        if (checkUpdatesOn()) runUpdateCheck();
    }

    void updRedraw() {
        if (page == 6) refresh();
        else showUpdState();
    }

    void runUpdateCheck() {
        if (updBusy) return;
        updBusy = true;
        updError = null;
        updRedraw();
        final String cur = appVersion();
        new Thread(new Runnable() { public void run() {
            final Update.Result r = Update.check(cur);
            runOnUiThread(new Runnable() { public void run() {
                updBusy = false;
                updChecked = true;
                updResult = r;
                updRedraw();
            } });
        } }).start();
    }

    void runUpdateDownload() {
        final Update.Result r = updResult;
        if (updBusy || r == null || !r.haveTarget) return;
        updBusy = updDownloading = true;
        updError = null;
        updGot = 0;
        updTotal = r.asset.size;
        updCancel.set(false);
        refresh();
        final String cur = appVersion();
        new Thread(new Runnable() { public void run() {
            try (FileOutputStream o = new FileOutputStream(updMarker())) {
                o.write(cur.getBytes("UTF-8"));
            } catch (IOException ignored) {
            }
            final String err = Update.download(LauncherActivity.this, r.asset, cur, new Update.Progress() {
                long shown = -1;
                public void at(final long got, long total) {
                    long pct = got * 100 / Math.max(total, 1);
                    if (pct == shown) return;
                    shown = pct;
                    runOnUiThread(new Runnable() { public void run() {
                        updGot = got;
                        updRedraw();
                    } });
                }
            }, updCancel);
            runOnUiThread(new Runnable() { public void run() {
                updBusy = updDownloading = false;
                if (err != null) {
                    updError = err;
                    updMarker().delete();
                } else updCommitted = true;
                updRedraw();
            } });
        } }).start();
    }

    // PackageInstaller's answers to the committed session.
    void installStatus(Intent i) {
        int st = i.getIntExtra(android.content.pm.PackageInstaller.EXTRA_STATUS, -999);
        if (st == android.content.pm.PackageInstaller.STATUS_PENDING_USER_ACTION) {
            Intent confirm = (Intent) i.getParcelableExtra(Intent.EXTRA_INTENT);
            if (confirm != null) {
                confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                try {
                    startActivity(confirm);
                    return;
                } catch (Exception e) {
                    updError = "could not show Android's install prompt: " + e;
                }
            }
        } else if (st == android.content.pm.PackageInstaller.STATUS_SUCCESS) {
            return;  // this process normally ends before getting here; the user opens the app again
        } else if (st == android.content.pm.PackageInstaller.STATUS_FAILURE_ABORTED) {
            updError = "the install was cancelled";
        } else {
            String msg = i.getStringExtra(android.content.pm.PackageInstaller.EXTRA_STATUS_MESSAGE);
            updError = "Android did not install it" + (msg == null || msg.isEmpty() ? "" : ": " + msg)
                    + (st == android.content.pm.PackageInstaller.STATUS_FAILURE_CONFLICT
                       || st == android.content.pm.PackageInstaller.STATUS_FAILURE_INCOMPATIBLE
                       ? " (an app signed differently, e.g. a build of your own, is installed: uninstall it first; "
                         + "your saves and settings in Internal storage/OpenCrown stay)" : "");
        }
        updCommitted = false;
        updMarker().delete();
        updRedraw();
    }

    boolean installAllowed() {
        return getPackageManager().canRequestPackageInstalls();
    }

    // The sidebar line under "Update" (and the gold dot when an update is waiting).
    void showUpdState() {
        if (updState == null) return;
        Update.Result r = updResult;
        String s;
        int c = DIM;
        boolean avail = false;
        if (updatedFrom != null) { s = "Updated to v" + appVersion(); c = OK; }
        else if (updDownloading) { s = "Downloading... " + (updTotal > 0 ? updGot * 100 / updTotal : 0) + "%"; c = GOLD; }
        else if (updBusy) s = "Checking...";
        else if (!updChecked) s = "v" + appVersion();
        else if (!r.ok) s = "Update check failed";
        else if (r.haveTarget) { s = "v" + r.latest + " available"; c = GOLD; avail = true; }
        else s = "Up to date (v" + appVersion() + ")";
        updState.setText(s);
        updState.setTextColor(c);
        tabs.get(6).setTextColor(page == 6 || avail ? GOLD : TEXT);
        tabs.get(6).setText(avail || updDownloading ? "Update  ●" : "Update");
    }

    void pageUpdate() {
        heading("Update");
        final Update.Result r = updResult;
        if (updatedFrom != null) para("Updated to v" + appVersion() + " (from v" + updatedFrom + ").").setTextColor(OK);
        LinearLayout vr = row();
        vr.addView(text("Current version: v" + appVersion(), 15, TEXT));
        String latest = updBusy && !updDownloading ? "checking..." : !updChecked ? "not checked yet"
                : !r.ok ? "unknown" : "v" + (r.latest.isEmpty() ? appVersion() : r.latest);
        span(vr, "     Latest version: " + latest, updChecked && r.ok ? TEXT : DIM).setTextSize(15);

        if (updChecked && !r.ok && !updBusy) para("Update check failed: " + r.error).setTextColor(BAD);
        else if (updChecked && r.ok && !updBusy) {
            if (r.haveTarget) para(String.format(Locale.ROOT, "Update available: v%s (%s, %.1f MB)", r.latest,
                    r.asset.name, r.asset.size / 1048576.0)).setTextColor(GOLD);
            else para("You are up to date.").setTextColor(OK);
        }
        boolean canInstall = updChecked && r.ok && r.haveTarget;
        if (canInstall && r.asset.sha256.isEmpty()) {
            para("The release lists no SHA-256 checksum for " + r.asset.name + ", so it is not installed "
                    + "automatically. Get it from the release page.").setTextColor(WARN);
            canInstall = false;
        }
        final boolean needAllow = canInstall && !installAllowed();
        if (needAllow)
            para("Android needs your OK once before OpenCrown Mobile can install its own updates: tap Allow, turn on "
                    + "\"Allow from this source\", then come back.").setTextColor(WARN);

        LinearLayout br = row();
        Button first = null;
        if (needAllow) {
            first = smallButton(br, "Allow", new View.OnClickListener() { public void onClick(View v) {
                try {
                    startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + getPackageName())));
                } catch (Exception e) {
                    updError = "could not open the setting: " + e;
                    refresh();
                }
            } });
        } else if (canInstall) {
            first = smallButton(br, updDownloading ? "Downloading..." : updCommitted ? "Installing..." : "Download and install",
                    new View.OnClickListener() { public void onClick(View v) { runUpdateDownload(); } });
            first.setEnabled(!updBusy && !updCommitted);
        }
        Button again = smallButton(br, updBusy && !updDownloading ? "Checking..." : "Check again", new View.OnClickListener() {
            public void onClick(View v) { updatedFrom = null; runUpdateCheck(); }
        });
        again.setEnabled(!updBusy);
        ((LinearLayout.LayoutParams) (first != null ? first : again).getLayoutParams()).leftMargin = 0;
        smallButton(br, "Open release page", new View.OnClickListener() { public void onClick(View v) {
            String url = r != null && r.page.startsWith("https://") ? r.page : Update.RELEASES_PAGE;
            try {
                startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
            } catch (Exception ignored) {
            }
        } });

        if (updDownloading || (updGot > 0 && updError != null)) {
            android.widget.ProgressBar pb = new android.widget.ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
            pb.setMax(1000);
            pb.setProgress(updTotal > 0 ? (int) (updGot * 1000 / updTotal) : 0);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, dp(12));
            lp.topMargin = dp(10);
            content.addView(pb, lp);
            para(String.format(Locale.ROOT, "%.1f / %.1f MB", updGot / 1048576.0, updTotal / 1048576.0)).setTextColor(DIM);
        }
        if (updCommitted && updError == null)
            para("Downloaded and checked. Confirm the update in Android's prompt, then open OpenCrown Mobile again.")
                    .setTextColor(OK);
        if (updError != null) para("Update failed: " + updError).setTextColor(BAD);

        if (haveAccess()) {
            android.widget.CheckBox cb = new android.widget.CheckBox(this);
            cb.setText("Check for updates when the launcher starts");
            cb.setTextColor(TEXT);
            cb.setChecked(checkUpdatesOn());
            cb.setOnCheckedChangeListener(new android.widget.CompoundButton.OnCheckedChangeListener() {
                public void onCheckedChanged(android.widget.CompoundButton b, boolean on) {
                    new Ini(new File(root, "launcher.ini")).set("check_updates", on ? "true" : "false");
                }
            });
            content.addView(cb);
        }
        para("Updates come from the OpenCrown GitHub releases. Your saves and settings in Internal storage/OpenCrown "
                + "are not touched.").setTextColor(DIM);

        if (updChecked && r.ok && r.haveTarget) {
            sub("What's new");
            for (String[] l : Update.noteLines(r.notes)) {
                if (l[0].isEmpty()) { content.addView(new View(this), new LinearLayout.LayoutParams(-1, dp(6))); continue; }
                boolean top = l[0].equals("H"), h = top || l[0].equals("h");
                TextView t = text(l[1], top ? 17 : h ? 15 : 14, h ? GOLD : TEXT);
                if (h) t.setTypeface(Typeface.SERIF, Typeface.BOLD);
                if (h) t.setPadding(0, dp(top ? 6 : 3), 0, dp(1));
                else t.setPadding(0, dp(1), 0, dp(1));
                content.addView(t);
            }
        }
    }

    // --- play ------------------------------------------------------------------------------------------------------
    // The game is already running (minimised, or reopened from the app icon / recents): return to it.
    boolean resumeGame() {
        if (!OpenCrownActivity.gameRunning()) return false;
        Intent i = new Intent(this, OpenCrownActivity.class);
        i.addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT);
        startActivity(i);
        finish();
        return true;
    }

    void startGame() {
        if (resumeGame()) return;
        if (dump == null || !dump.ready()) return;
        ArrayList<String> args = new ArrayList<>();
        args.add(new File(dump.update.dir, "eboot.bin").getAbsolutePath());
        args.add("--app0");
        args.add(dump.update.dir.getAbsolutePath());
        args.add("--app0");
        args.add(dump.game.dir.getAbsolutePath());
        args.add("--root");
        args.add(root.getAbsolutePath());
        args.add("--lobby");
        args.add(lobbyUrl());
        args.add("--online-name");
        args.add(playerName());
        if (!upnpOn()) args.add("--no-upnp");
        Intent i = new Intent(this, OpenCrownActivity.class);
        i.putExtra(OpenCrownActivity.EXTRA_ARGS, args.toArray(new String[0]));
        if (OpenCrownActivity.mainFinished()) { RestartActivity.restartInto(this, i); return; }  // after a failed start
        startActivity(i);
        finish();
    }
}
