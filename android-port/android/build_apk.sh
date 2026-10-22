#!/usr/bin/env bash
# Builds the OpenCrown Mobile APK (arm64-v8a) without Gradle: NDK CMake build of libmain.so + libSDL3.so, SDL's Java
# glue + our activity through javac and d8, manifest through aapt2, then zipalign and apksigner (debug key).
#
#   android-port/android/build_apk.sh [install]      install = adb install -r afterwards
#
# Paths default to the dev PC layout (README "Android build"); override with the environment variables below.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd -W)"  # C:/... form: the tools are Windows programs
ROOT="$(cd "$HERE/.." && pwd -W)"
TOOLS="${DCP_TOOLS:-E:/DecompWork/tools}"
SDK="${ANDROID_SDK:-$TOOLS/android-sdk}"
NDK="${ANDROID_NDK:-$SDK/ndk/27.3.13750724}"
BT="$SDK/build-tools/35.0.0"
JAR="$SDK/platforms/android-35/android.jar"
SDL="${DCP_SDL3_SOURCE:-$TOOLS/SDL3-3.4.18}"
export JAVA_HOME="${JAVA_HOME:-$TOOLS/jdk-21.0.12.1+1}"
JBIN="$JAVA_HOME/bin"
KEYSTORE="${DCP_DEBUG_KEYSTORE:-$TOOLS/android-debug.keystore}"
OUT="$ROOT/build-android"
APK="$OUT/OpenCrownMobile.apk"

echo "== native (NDK, arm64-v8a)"
cmake -S "$ROOT" -B "$OUT/native" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DANDROID_STL=c++_static \
  -DDCP_SDL3_SOURCE="$SDL" \
  -DFETCHCONTENT_SOURCE_DIR_BOOST="$TOOLS/boost-1.86.0" \
  -DFETCHCONTENT_SOURCE_DIR_DYNARMIC="$TOOLS/dynarmic-1fcd216467c889644921db949c218cf46f59ff3b" \
  -DFETCHCONTENT_SOURCE_DIR_FREETYPE="$TOOLS/freetype-VER-2-13-3" \
  -DFETCHCONTENT_SOURCE_DIR_VULKAN_HEADERS="$TOOLS/vulkan-headers-1.3.296" \
  -DFETCHCONTENT_SOURCE_DIR_SPIRV_HEADERS="$TOOLS/spirv-headers-1.3.296.0" \
  -DFETCHCONTENT_SOURCE_DIR_MBEDTLS="$TOOLS/mbedtls-3.6.2" \
  -DFETCHCONTENT_SOURCE_DIR_MINIUPNPC="$TOOLS/miniupnp-miniupnpc_2_2_8" > "$OUT.configure.log"
# libmain.so links dcp_net (the online HLE); the target is listed so a net-only change still builds it.
cmake --build "$OUT/native" --target main SDL3-shared dcp_net

echo "== java (SDL glue + OpenCrownActivity)"
rm -rf "$OUT/classes" "$OUT/dex" && mkdir -p "$OUT/classes" "$OUT/dex"
find "$SDL/android-project/app/src/main/java" "$HERE/java" -name '*.java' | sed 's/.*/"&"/' > "$OUT/java_sources.txt"
"$JBIN/javac" -nowarn -Xlint:-options -source 8 -target 8 -encoding UTF-8 -bootclasspath "$JAR" \
  -d "$OUT/classes" @"$OUT/java_sources.txt"
(cd "$OUT/classes" && find . -name '*.class' > "$OUT/class_list.txt")
(cd "$OUT/classes" && "$BT/d8.bat" --release --min-api 28 --lib "$JAR" --output "$OUT/dex" @"$OUT/class_list.txt")

echo "== package"
# Dev builds are debuggable (adb run-as copies the dump into the app's files); DCP_RELEASE=1 leaves that out.
DEBUG_FLAG=--debug-mode
[ "${DCP_RELEASE:-}" = 1 ] && DEBUG_FLAG=
# android/res: the launcher icon (mipmap-*/ic_launcher.png, 48..192 px).
rm -f "$OUT/res.zip"
"$BT/aapt2.exe" compile --dir "$HERE/res" -o "$OUT/res.zip"
"$BT/aapt2.exe" link -o "$OUT/base.apk" --manifest "$HERE/AndroidManifest.xml" -I "$JAR" $DEBUG_FLAG \
  --min-sdk-version 28 --target-sdk-version 35 "$OUT/res.zip"
LIBSDL="$(find "$OUT/native/sdl3" -name libSDL3.so | head -1)"
LIBMAIN="$OUT/native/libmain.so"
# Release: debug info out (~60 MB, and it holds this PC's source paths); the symbol table stays for simpleperf.
if [ "${DCP_RELEASE:-}" = 1 ]; then
  STRIP="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe"
  mkdir -p "$OUT/stripped"
  "$STRIP" --strip-debug -o "$OUT/stripped/libmain.so" "$LIBMAIN"
  "$STRIP" --strip-debug -o "$OUT/stripped/libSDL3.so" "$LIBSDL"
  LIBMAIN="$OUT/stripped/libmain.so" LIBSDL="$OUT/stripped/libSDL3.so"
fi
python -I "$HERE/pack_apk.py" "$OUT/base.apk" "$OUT/unaligned.apk" "$OUT/dex/classes.dex" "$LIBMAIN" "$LIBSDL"
"$BT/zipalign.exe" -f -P 16 4 "$OUT/unaligned.apk" "$OUT/aligned.apk"
if [ ! -f "$KEYSTORE" ]; then
  "$JBIN/keytool" -genkeypair -keystore "$KEYSTORE" -storepass android -keypass android -alias debug \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=OpenCrown debug" > /dev/null
fi
"$BT/apksigner.bat" sign --ks "$KEYSTORE" --ks-pass pass:android --ks-key-alias debug --out "$APK" "$OUT/aligned.apk"
echo "== $APK ($(du -h "$APK" | cut -f1))"

if [ "${1:-}" = install ]; then
  "$SDK/platform-tools/adb.exe" install -r "$APK"
fi
