# Adds classes.dex and the native libraries to the aapt2 output: pack_apk.py <base.apk> <out.apk> <classes.dex> <lib.so>...
# Libraries are stored uncompressed (the manifest sets extractNativeLibs=false); zipalign -P 16 page-aligns them next.
import os
import sys
import zipfile

base, out, dex, *libs = sys.argv[1:]
with zipfile.ZipFile(base) as src, zipfile.ZipFile(out, "w") as dst:
    for item in src.infolist():
        dst.writestr(item, src.read(item.filename))
    dst.write(dex, "classes.dex", compress_type=zipfile.ZIP_DEFLATED)
    for lib in libs:
        dst.write(lib, "lib/arm64-v8a/" + os.path.basename(lib), compress_type=zipfile.ZIP_STORED)
