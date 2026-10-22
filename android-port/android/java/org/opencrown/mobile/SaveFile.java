package org.opencrown.mobile;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.zip.CRC32;

// Dragon's Crown SAVE0.DAT checks (same container on Vita and PS4): 0x20eb18 bytes, u32 CRC-32 of [4, end) at 0,
// "DCSD" at 4, u32 version at 8. The Vita build loads versions 0x24 / 0x25 (validator 0x8136da62); Pro writes 0x26.
final class SaveFile {
    static final int SIZE = 0x20eb18, PRO_VERSION = 0x26, SLOT_PARAM_SIZE = 0x34c;

    int version;
    String problem;

    // The Vita game with update 1.09 loads versions 0x24, 0x25 and 0x26 (= PC / PS4 / PS5 saves) directly (asm, its
    // save validator), so no conversion is needed.
    boolean supported() { return version == 0x24 || version == 0x25 || version == PRO_VERSION; }

    static SaveFile parse(byte[] b) {
        SaveFile s = new SaveFile();
        if (b.length != SIZE) { s.problem = "wrong size (" + b.length + " bytes)"; return s; }
        ByteBuffer bb = ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN);
        if (b[4] != 'D' || b[5] != 'C' || b[6] != 'S' || b[7] != 'D') { s.problem = "no DCSD header"; return s; }
        CRC32 crc = new CRC32();
        crc.update(b, 4, SIZE - 4);
        if ((int) crc.getValue() != bb.getInt(0)) { s.problem = "is damaged (checksum mismatch)"; return s; }
        s.version = bb.getInt(8);
        return s;
    }

    // null when there is no file.
    static SaveFile inspect(File f) {
        if (!f.isFile()) return null;
        try (FileInputStream in = new FileInputStream(f)) {
            SaveFile s = parse(readAll(in, SIZE + 1));
            if (s.problem == null && !s.supported()) s.problem = "has version 0x" + Integer.toHexString(s.version)
                    + ", which the Vita game cannot load";
            return s;
        } catch (IOException e) {
            SaveFile s = new SaveFile();
            s.problem = "cannot be read";
            return s;
        }
    }

    static byte[] readAll(InputStream in, int limit) throws IOException {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        byte[] buf = new byte[1 << 16];
        for (int r; (r = in.read(buf)) > 0;) {
            o.write(buf, 0, r);
            if (o.size() > limit) break;
        }
        return o.toByteArray();
    }
}
