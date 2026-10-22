package org.opencrown.mobile;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

// input.ini "key = value" lines (the format src/platform/sdl_app.cpp reads): get one value, or set it in place
// keeping every other line and comment.
final class Ini {
    private final File file;

    Ini(File f) { file = f; }

    private List<String> lines() {
        List<String> out = new ArrayList<>();
        if (!file.isFile()) return out;
        try (FileInputStream in = new FileInputStream(file)) {
            byte[] b = SaveFile.readAll(in, 1 << 20);
            for (String l : new String(b, StandardCharsets.UTF_8).split("\r?\n", -1)) out.add(l);
            if (!out.isEmpty() && out.get(out.size() - 1).isEmpty()) out.remove(out.size() - 1);
        } catch (IOException ignored) {
        }
        return out;
    }

    private static String keyOf(String line) {
        String t = line.trim();
        int eq = t.indexOf('=');
        return t.startsWith("#") || eq < 0 ? null : t.substring(0, eq).trim();
    }

    String get(String key) {
        for (String l : lines())
            if (key.equals(keyOf(l))) return l.substring(l.indexOf('=') + 1).trim();
        return null;
    }

    void set(String key, String value) {
        List<String> ls = lines();
        boolean done = false;
        for (int i = 0; i < ls.size(); i++)
            if (key.equals(keyOf(ls.get(i)))) { ls.set(i, key + " = " + value); done = true; }
        if (!done) ls.add(key + " = " + value);
        StringBuilder sb = new StringBuilder();
        for (String l : ls) sb.append(l).append('\n');
        try (FileOutputStream o = new FileOutputStream(file)) {
            o.write(sb.toString().getBytes(StandardCharsets.UTF_8));
        } catch (IOException ignored) {
        }
    }
}
