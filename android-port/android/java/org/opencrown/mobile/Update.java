package org.opencrown.mobile;

import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

// Launcher self-updater, as the PC / Linux launchers' (port/tools/launcher/update.h, linux-port/src/net/update.h):
// GitHub releases API -> the highest OpenCrownMobile-v<ver>[-beta]-arm64.apk newer than this app, the notes of the
// releases that brought each newer APK, then the download streamed into a PackageInstaller session with its size and
// SHA-256 checked against the API before the session is committed (Android asks the user to confirm the update; the
// APK must carry the same signature as the installed app). Releases are tagged with the PC version, so the mobile
// version comes from the asset name. Blocking calls: run check() / download() off the UI thread.
final class Update {
    static final String API_URL = "https://api.github.com/repos/Nezara/OpenCrown-PC/releases?per_page=20";
    static final String RELEASES_PAGE = "https://github.com/Nezara/OpenCrown-PC/releases";
    static final String ACTION_STATUS = "org.opencrown.mobile.UPDATE_STATUS";

    private static final Pattern ASSET = Pattern.compile("OpenCrownMobile-v(\\d+(?:\\.\\d+){0,3})(-beta)?-arm64\\.apk");

    // "0.1.1" / "v1.0" / "0.1.1-beta" -> 4 numeric parts (missing ones 0), null for anything else.
    static int[] parseVersion(String s) {
        if (s == null) return null;
        s = s.trim();
        if (s.startsWith("v") || s.startsWith("V")) s = s.substring(1);
        int dash = s.indexOf('-');
        if (dash >= 0) s = s.substring(0, dash);
        String[] p = s.split("\\.", -1);
        if (p.length == 0 || p.length > 4) return null;
        int[] v = new int[4];
        for (int i = 0; i < p.length; i++) {
            if (p[i].isEmpty() || p[i].length() > 6) return null;
            for (int k = 0; k < p[i].length(); k++) if (!Character.isDigit(p[i].charAt(k))) return null;
            v[i] = Integer.parseInt(p[i]);
        }
        return v;
    }

    static int compare(int[] a, int[] b) {
        for (int i = 0; i < 4; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
        return 0;
    }

    static final class Asset {
        String name = "", url = "", sha256 = "", version = "";  // version as in the name: "0.1.1-beta"
        int[] ver;
        long size = -1;
    }

    static final class Result {
        boolean ok, haveTarget;
        String error = "", current = "", latest = "", page = RELEASES_PAGE;
        Asset asset;
        String notes = "";  // Markdown bodies of the releases that brought each newer APK, newest first
    }

    private static final class Rel {
        String tag, name, body, page, published;
        Asset apk;
    }

    // Pure: the target among the API's releases (drafts / prereleases ignored).
    static Result evaluate(String json, String current) {
        Result r = new Result();
        r.current = current;
        List<Rel> rels = new ArrayList<>();
        try {
            JSONArray a = new JSONArray(json);
            for (int i = 0; i < a.length(); i++) {
                JSONObject o = a.optJSONObject(i);
                if (o == null || o.optBoolean("draft") || o.optBoolean("prerelease")) continue;
                Rel rel = new Rel();
                rel.tag = o.optString("tag_name");
                rel.name = o.optString("name");
                rel.body = o.isNull("body") ? "" : o.optString("body");
                rel.page = o.optString("html_url");
                rel.published = o.optString("published_at");
                JSONArray as = o.optJSONArray("assets");
                for (int k = 0; as != null && k < as.length(); k++) {
                    JSONObject x = as.optJSONObject(k);
                    if (x == null) continue;
                    Matcher m = ASSET.matcher(x.optString("name"));
                    if (!m.matches()) continue;
                    Asset s = new Asset();
                    s.name = x.optString("name");
                    s.url = x.optString("browser_download_url");
                    s.size = x.optLong("size", -1);
                    s.version = m.group(1) + (m.group(2) == null ? "" : m.group(2));
                    s.ver = parseVersion(m.group(1));
                    String d = x.isNull("digest") ? "" : x.optString("digest");
                    if (d.startsWith("sha256:") && d.length() == 71) s.sha256 = d.substring(7).toLowerCase(Locale.ROOT);
                    if (s.ver != null && !s.url.isEmpty()) rel.apk = s;
                }
                if (rel.apk != null) rels.add(rel);
            }
        } catch (Exception e) {
            r.error = "unexpected answer from the update server";
            return r;
        }
        r.ok = true;
        int[] cur = parseVersion(current);
        // Oldest release first: an APK carried on unchanged by later PC releases is credited to the release that
        // brought it, so its notes are the ones shown.
        Collections.sort(rels, new Comparator<Rel>() {
            public int compare(Rel a, Rel b) { return a.published.compareTo(b.published); }
        });
        List<Rel> newer = new ArrayList<>();
        for (Rel rel : rels) {
            if (cur != null && compare(rel.apk.ver, cur) <= 0) continue;
            boolean seen = false;
            for (Rel n : newer) seen |= compare(n.apk.ver, rel.apk.ver) == 0;
            if (!seen) newer.add(rel);
        }
        if (newer.isEmpty()) return r;
        Collections.sort(newer, new Comparator<Rel>() {
            public int compare(Rel a, Rel b) { return Update.compare(b.apk.ver, a.apk.ver); }
        });
        Rel top = newer.get(0);
        r.haveTarget = true;
        r.asset = top.apk;
        r.latest = top.apk.version;
        if (!top.page.isEmpty()) r.page = top.page;
        StringBuilder notes = new StringBuilder();
        for (Rel rel : newer) {
            String body = mobileNotes(rel.body);
            notes.append("## OpenCrown Mobile v").append(rel.apk.version).append("\n\n")
                    .append(body.isEmpty() ? "(no Android-specific notes)" : body).append("\n\n");
        }
        r.notes = notes.toString().trim();
        return r;
    }

    private static final Pattern MOBILE_WORD = Pattern.compile("\\b(Mobile|Android)\\b");

    // Releases are shared with the PC builds: keep only what concerns OpenCrown Mobile. Whole sections whose heading
    // names Mobile / Android, and from the other sections the top-level bullets that name them (e.g. "(Windows, Linux
    // and Mobile.)"), under their section heading. The text before the first heading (intro, downloads) is dropped.
    static String mobileNotes(String body) {
        StringBuilder out = new StringBuilder();
        String heading = null;           // current section's heading line; null before the first one
        boolean mobileSection = false, headingOut = false;
        StringBuilder bullet = null;     // the top-level bullet being collected in a non-Mobile section
        for (String line : (body + "\n").split("\r?\n", -1)) {
            String t = line.trim();
            boolean isHeading = t.matches("#{1,6} .*");
            boolean topBullet = !isHeading && line.matches("[-*+] .*");
            if (bullet != null && (isHeading || topBullet || t.isEmpty())) {  // flush the finished bullet
                if (MOBILE_WORD.matcher(bullet).find()) {
                    if (!headingOut) out.append("\n").append(heading).append("\n");
                    headingOut = true;
                    out.append(bullet);
                }
                bullet = null;
            }
            if (isHeading) {
                heading = t;
                mobileSection = MOBILE_WORD.matcher(t).find();
                headingOut = mobileSection;
                if (mobileSection) out.append("\n").append(t).append("\n");
                continue;
            }
            if (heading == null) continue;
            if (mobileSection) out.append(line).append("\n");
            else if (topBullet) bullet = new StringBuilder(line).append("\n");
            else if (bullet != null) bullet.append(line).append("\n");
        }
        return out.toString().trim();
    }

    // Only GitHub's hosts, https only, redirects followed by hand so every hop is checked.
    static boolean hostAllowed(URL u) {
        if (!"https".equals(u.getProtocol())) return false;
        String h = u.getHost().toLowerCase(Locale.ROOT);
        return h.equals("api.github.com") || h.equals("github.com") || h.endsWith(".githubusercontent.com");
    }

    private static HttpURLConnection open(String url, String version, String accept) throws IOException {
        URL u = new URL(url);
        for (int hop = 0; hop < 6; hop++) {
            if (!hostAllowed(u)) throw new IOException("refusing to download from " + u.getHost());
            HttpURLConnection c = (HttpURLConnection) u.openConnection();
            c.setInstanceFollowRedirects(false);
            c.setConnectTimeout(10000);
            c.setReadTimeout(20000);
            c.setRequestProperty("User-Agent", "OpenCrown-Mobile-Launcher/" + version);
            c.setRequestProperty("Accept", accept);
            int code = c.getResponseCode();
            if (code >= 300 && code < 400) {
                String loc = c.getHeaderField("Location");
                c.disconnect();
                if (loc == null) throw new IOException("redirect without a target");
                u = new URL(u, loc);
                continue;
            }
            if (code != 200) {
                c.disconnect();
                throw new IOException("the server answered HTTP " + code);
            }
            return c;
        }
        throw new IOException("too many redirects");
    }

    static Result check(String current) {
        Result bad = new Result();
        bad.current = current;
        try {
            HttpURLConnection c = open(API_URL, current, "application/vnd.github+json");
            ByteArrayOutputStream b = new ByteArrayOutputStream();
            try (InputStream in = c.getInputStream()) {
                byte[] buf = new byte[16384];
                for (int n; (n = in.read(buf)) > 0;) {
                    b.write(buf, 0, n);
                    if (b.size() > (16 << 20)) throw new IOException("the update server's answer is too large");
                }
            } finally {
                c.disconnect();
            }
            return evaluate(b.toString("UTF-8"), current);
        } catch (Exception e) {
            bad.error = e.getMessage() != null ? e.getMessage() : e.toString();
            return bad;
        }
    }

    interface Progress { void at(long got, long total); }

    // Streams the APK into a new install session, checks size + SHA-256, then commits it; the outcome arrives as an
    // ACTION_STATUS broadcast to this app (PackageInstaller.EXTRA_STATUS). Returns null when committed, else why not
    // (the session is abandoned then, nothing is changed).
    static String download(Context ctx, Asset a, String current, Progress progress, AtomicBoolean cancel) {
        if (a.sha256.isEmpty()) return "the release lists no SHA-256 digest for " + a.name + ": not installing it automatically";
        if (a.size <= 0) return "the release lists no size for " + a.name;
        PackageInstaller pi = ctx.getPackageManager().getPackageInstaller();
        int id = -1;
        PackageInstaller.Session s = null;
        try {
            PackageInstaller.SessionParams sp = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
            sp.setAppPackageName(ctx.getPackageName());
            sp.setSize(a.size);
            id = pi.createSession(sp);
            s = pi.openSession(id);
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            long got = 0;
            HttpURLConnection c = open(a.url, current, "application/octet-stream");
            try (InputStream in = c.getInputStream(); OutputStream out = s.openWrite("base.apk", 0, a.size)) {
                byte[] buf = new byte[65536];
                for (int n; (n = in.read(buf)) > 0;) {
                    if (cancel != null && cancel.get()) throw new IOException("cancelled");
                    if (got + n > a.size) throw new IOException("the download is larger than the release says");
                    out.write(buf, 0, n);
                    md.update(buf, 0, n);
                    got += n;
                    if (progress != null) progress.at(got, a.size);
                }
                s.fsync(out);
            } finally {
                c.disconnect();
            }
            if (got != a.size) throw new IOException("size mismatch: got " + got + " bytes, the release says " + a.size);
            StringBuilder hex = new StringBuilder();
            for (byte x : md.digest()) hex.append(String.format(Locale.ROOT, "%02x", x & 0xff));
            if (!hex.toString().equals(a.sha256)) throw new IOException("SHA-256 mismatch: the download is damaged (nothing was changed)");
            Intent done = new Intent(ACTION_STATUS).setPackage(ctx.getPackageName());
            PendingIntent pend = PendingIntent.getBroadcast(ctx, id, done,
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE);  // the installer adds its extras
            s.commit(pend.getIntentSender());
            s.close();
            return null;
        } catch (Exception e) {
            if (s != null) s.abandon();
            else if (id >= 0) pi.abandonSession(id);
            return e.getMessage() != null ? e.getMessage() : e.toString();
        }
    }

    // Release notes Markdown -> display lines {kind, text}: kind "H" / "h" heading, "b" bullet, "t" text, "" blank. Inline
    // **bold**, `code` and [text](url) become plain text (as the PC launcher's notes_from_markdown).
    static List<String[]> noteLines(String md) {
        List<String[]> out = new ArrayList<>();
        boolean fence = false;
        for (String line : md.split("\r?\n", -1)) {
            String t = line.trim();
            if (t.startsWith("```") || t.startsWith("~~~")) { fence = !fence; continue; }
            if (fence) { out.add(new String[] {"t", line}); continue; }
            if (t.isEmpty() || t.matches("[-*_ ]{3,}")) {
                if (!out.isEmpty() && !out.get(out.size() - 1)[0].isEmpty()) out.add(new String[] {"", ""});
                continue;
            }
            if (t.startsWith("<!--")) continue;
            if (t.matches("#{1,6} .*"))  // "H" = a release (## OpenCrown Mobile v...), "h" = a section inside it
                out.add(new String[] {t.matches("#{1,2} .*") ? "H" : "h", inline(t.replaceFirst("^#+ ", "").replaceAll("[ #]+$", ""))});
            else if (t.matches("[-*+] .*")) {
                int indent = (line.length() - line.replaceAll("^\\s+", "").length()) / 2;
                StringBuilder pre = new StringBuilder();
                for (int i = 0; i < Math.min(indent, 4); i++) pre.append("    ");
                out.add(new String[] {"b", pre + "• " + inline(t.substring(2).replaceFirst("^\\[[ x]\\] ", ""))});
            } else out.add(new String[] {"t", inline(t.replaceFirst("^(> ?)+", ""))});
        }
        while (!out.isEmpty() && out.get(out.size() - 1)[0].isEmpty()) out.remove(out.size() - 1);
        return out;
    }

    static String inline(String s) {
        return s.replaceAll("!?\\[([^\\]]*)\\]\\([^)]*\\)", "$1").replace("**", "").replace("__", "").replace("`", "")
                .replaceAll("\\\\(\\p{Punct})", "$1");
    }
}
