#!/bin/sh
# Builds dist/OpenCrownPC-v<version>-win64.zip (Git Bash on Windows): the folder OpenCrownPC/ with the launcher,
# dcp_run.exe, SDL3.dll, README.txt, THIRD-PARTY-NOTICES.txt and licenses/ (text files from port/packaging/, CRLF).
# Zipped with Windows' own %SystemRoot%\System32\tar.exe (forward-slash entry names; the launcher's updater
# extracts with the same tar.exe and looks for "OpenCrownPC Launcher.exe" in the zip).
# Usage: port/packaging/make-zip.sh [--build DIR] [--out DIR] [--version X.Y.Z]
#   defaults: port/build, dist/, the version the build was configured with (OPENCROWN_VERSION in port/CMakeLists.txt,
#   or OPENCROWN_VERSION_OVERRIDE from the build's CMakeCache.txt for test builds).
# Prints the zip path, size and SHA-256 (the GitHub release shows the same digest).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
port=$(cd "$here/.." && pwd)
build="$port/build"
out="$port/../dist"
version=""
while [ $# -gt 0 ]; do
    case "$1" in
    --build) build="$2"; shift 2 ;;
    --out) out="$2"; shift 2 ;;
    --version) version="$2"; shift 2 ;;
    *) echo "usage: $0 [--build DIR] [--out DIR] [--version X.Y.Z]" >&2; exit 2 ;;
    esac
done
if [ -z "$version" ]; then
    version=$(sed -n 's/^OPENCROWN_VERSION_OVERRIDE:STRING=\(.*\)$/\1/p' "$build/CMakeCache.txt" 2>/dev/null || true)
    [ -n "$version" ] || version=$(sed -n 's/^set(OPENCROWN_VERSION "\([^"]*\)").*/\1/p' "$port/CMakeLists.txt")
fi
case "$version" in
'' | *[!0-9.]*) echo "bad version '$version'" >&2; exit 1 ;;
esac
tar_exe="${SYSTEMROOT:-${SystemRoot:-C:/Windows}}/System32/tar.exe"
[ -x "$tar_exe" ] || { echo "Windows tar.exe not found ($tar_exe)" >&2; exit 1; }
for f in "OpenCrownPC Launcher.exe" dcp_run.exe SDL3.dll; do
    [ -f "$build/$f" ] || { echo "missing $build/$f (build first)" >&2; exit 1; }
done

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
dir="$stage/OpenCrownPC"
mkdir -p "$dir/licenses"
cp "$build/OpenCrownPC Launcher.exe" "$build/dcp_run.exe" "$build/SDL3.dll" "$dir/"
crlf() { sed 's/\r*$/\r/' "$1" > "$2"; }  # Notepad-friendly line ends
crlf "$here/README.txt" "$dir/README.txt"
crlf "$here/THIRD-PARTY-NOTICES.txt" "$dir/THIRD-PARTY-NOTICES.txt"
for f in "$here"/licenses/*.txt; do crlf "$f" "$dir/licenses/$(basename "$f")"; done

mkdir -p "$out"
out=$(cd "$out" && pwd)
zip="$out/OpenCrownPC-v$version-win64.zip"
rm -f "$zip"
(cd "$stage" && "$tar_exe" -a -c -f "$(cygpath -w "$zip")" OpenCrownPC)
echo "$zip"
echo "size $(wc -c < "$zip") bytes"
echo "sha256 $(sha256sum "$zip" | cut -d' ' -f1)"
