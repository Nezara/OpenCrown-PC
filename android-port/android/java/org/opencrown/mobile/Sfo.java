package org.opencrown.mobile;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;

// Reads a PSF (param.sfo) key table: "\0PSF", version, key table offset, data table offset, entry count, then
// 16-byte entries {u16 key offset, u16 format (0x0204 utf-8, 0x0404 u32), u32 length, u32 max, u32 data offset}.
final class Sfo {
    private final Map<String, String> values = new HashMap<>();

    String get(String key) {
        String v = values.get(key);
        return v == null ? "" : v;
    }

    static Sfo read(File f) {
        if (!f.isFile() || f.length() > (1 << 20)) return null;
        byte[] b = new byte[(int) f.length()];
        try (FileInputStream in = new FileInputStream(f)) {
            int n = 0;
            while (n < b.length) {
                int r = in.read(b, n, b.length - n);
                if (r < 0) return null;
                n += r;
            }
        } catch (IOException e) {
            return null;
        }
        try {
            ByteBuffer bb = ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN);
            if (bb.getInt(0) != 0x46535000) return null;
            int keys = bb.getInt(8), data = bb.getInt(12), count = bb.getInt(16);
            Sfo s = new Sfo();
            for (int i = 0; i < count; i++) {
                int e = 20 + 16 * i;
                int ko = bb.getShort(e) & 0xffff, fmt = bb.getShort(e + 2) & 0xffff, len = bb.getInt(e + 4),
                        dof = bb.getInt(e + 12);
                int k0 = keys + ko, k1 = k0;
                while (b[k1] != 0) k1++;
                String key = new String(b, k0, k1 - k0, StandardCharsets.UTF_8);
                int d = data + dof;
                String val;
                if (fmt == 0x0404) val = Integer.toString(bb.getInt(d));
                else {
                    int end = d;
                    while (end < d + len && b[end] != 0) end++;
                    val = new String(b, d, end - d, StandardCharsets.UTF_8);
                }
                s.values.put(key, val);
            }
            return s;
        } catch (IndexOutOfBoundsException e) {
            return null;
        }
    }
}
