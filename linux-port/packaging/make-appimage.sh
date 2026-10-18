#!/bin/sh
# Builds dist/OpenCrown-Linux-<version>-x86_64.AppImage (launcher + dcp_run, no game files) and dist/README.txt.
# Run on Ubuntu 22.04 (glibc 2.35 is then the minimum on the player's side; SteamOS 3.5+ is newer). Needs the build.sh
# packages plus file + desktop-file-utils. appimagetool (pinned version, SHA-256 checked) is downloaded once into
# ~/.cache/opencrown/ unless APPIMAGETOOL points at a copy.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
build=${OC_RELEASE_BUILD:-$HOME/oc-build-release}  # outside /mnt/c under WSL: much faster
ver=$(sed -n 's/^set(OPENCROWN_VERSION "\(.*\)").*/\1/p' "$here/CMakeLists.txt")
# Updater end-to-end tests only: OC_VERSION builds a fake release with another version (use its own OC_RELEASE_BUILD and
# OC_OUT so the real release build and dist/ stay untouched).
ver=${OC_VERSION:-$ver}
out=${OC_OUT:-$here/dist}
deps="$here/build/_deps"  # FetchContent sources, shared with ./build.sh

# 1. Release build: libstdc++ / libgcc linked in, so only libc / libm / ld-linux are needed at run time. SDL loads
#    X11 / Wayland / PulseAudio / PipeWire / ALSA / GL / udev from the system when it runs (never bundled).
if [ -z "$CXX" ] && command -v g++-12 >/dev/null 2>&1; then export CC=gcc-12 CXX=g++-12; fi
cmake -S "$here" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_BASE_DIR="$deps" \
    -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" -DOPENCROWN_VERSION_OVERRIDE="${OC_VERSION:-}"
cmake --build "$build" --target opencrown_launcher dcp_run -j"$(nproc)"

# 2. AppDir.
app="$build/AppDir"
rm -rf "$app"
mkdir -p "$app/usr/bin" "$app/usr/share/doc/opencrown/licenses"
for b in opencrown-launcher dcp_run; do
    cp "$build/$b" "$app/usr/bin/"
    strip "$app/usr/bin/$b"
done
cp "$here/packaging/AppRun" "$app/AppRun"
chmod +x "$app/AppRun"
cp "$here/packaging/opencrown.desktop" "$here/packaging/opencrown.svg" "$app/"
ln -s opencrown.svg "$app/.DirIcon"
cp "$here/packaging/README.txt" "$app/usr/share/doc/opencrown/"
lic="$app/usr/share/doc/opencrown/licenses"
cp "$deps/sdl3-src/LICENSE.txt" "$lic/SDL3.txt"
cp "$deps/sdl3-src/src/hidapi/LICENSE-bsd.txt" "$lic/SDL3-hidapi-BSD.txt"
cp "$deps/freetype-src/docs/FTL.TXT" "$lic/FreeType-FTL.txt"
cp "$deps/mbedtls-src/LICENSE" "$lic/MbedTLS-Apache-2.0.txt"
cp "$deps/miniupnpc-src/LICENSE" "$lic/miniupnpc-BSD-3.txt"
cp "$deps/imgui-src/LICENSE.txt" "$lic/DearImGui-MIT.txt"
cp "$here/packaging/THIRD-PARTY-NOTICES.txt" "$app/usr/share/doc/opencrown/"

# 3. appimagetool (pinned).
tool=${APPIMAGETOOL:-$HOME/.cache/opencrown/appimagetool-1.9.1-x86_64.AppImage}
sha=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
if [ ! -f "$tool" ]; then
    mkdir -p "$(dirname "$tool")"
    curl -fL -o "$tool.part" https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage
    mv "$tool.part" "$tool"
fi
echo "$sha  $tool" | sha256sum -c - >/dev/null || { echo "appimagetool: SHA-256 mismatch, refusing to run it" >&2; exit 1; }
chmod +x "$tool"

# 4. AppImage + README next to it.
mkdir -p "$out"
name="OpenCrown-Linux-$ver-x86_64.AppImage"
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$tool" --no-appstream "$app" "$out/$name"
cp "$here/packaging/README.txt" "$out/README.txt"
echo "built $out/$name ($(stat -c %s "$out/$name") B)"
