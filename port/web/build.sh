#!/bin/sh
# build.sh <deps prefix> <build dir> <site dir>: the web build (emsdk's environment needed) and the site around it,
# ready to serve from <site dir>. The deps prefix is deps.sh's.
set -eu
P=$1; B=$2; OUT=$3
HERE=$(cd "$(dirname "$0")" && pwd)
PORT=$(cd "$HERE/.." && pwd)
ROOT=$(cd "$PORT/.." && pwd)

emcmake cmake -S "$PORT" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release -DGTA2_SETUP_DEPS_DIR="$P" \
    -DCMAKE_PREFIX_PATH="$P" -DCMAKE_FIND_ROOT_PATH="$P" ${GTA2_VERSION:+-DGTA2_VERSION=$GTA2_VERSION}
ninja -C "$B" gta2 reg2_import

rm -rf "$OUT"
mkdir -p "$OUT/icons"
cp -R "$HERE/site/." "$OUT/"
# The game and the importer in a folder named after their contents: a new build's page simply asks for new files,
# which the service worker may keep for good (sw.js).
T=$(mktemp -d)
cp "$B/reg2.js" "$B/reg2.wasm" "$B/reg2_import.js" "$B/reg2_import.wasm" "$OUT/import-worker.js" "$T/"
rm "$OUT/import-worker.js"
BUILD=build-$(cd "$T" && cat reg2.js reg2.wasm reg2_import.js reg2_import.wasm import-worker.js | sha256sum | cut -c1-12)
mkdir -p "$OUT/$BUILD"
mv "$T/"* "$OUT/$BUILD/"
rmdir "$T"
sed -i "s|@BUILD@|$BUILD|" "$OUT/app.js"
# The landing page's legal line is the site's only copy of it: the game page's footer takes it from there.
LEGAL=$(grep -m1 '<p class="legal">' "$HERE/home/index.html" | sed 's/^ *//')
[ -n "$LEGAL" ] || { echo "build.sh: no <p class=\"legal\"> in home/index.html" >&2; exit 1; }
awk -v legal="$LEGAL" 'index($0, "<!--@LEGAL@-->") { gsub(/&/, "\\\\&", legal); sub(/<!--@LEGAL@-->/, legal) } { print }' \
    "$OUT/index.html" > "$OUT/index.html.new"
mv "$OUT/index.html.new" "$OUT/index.html"
cp "$ROOT/docs/images/reg2_logo_black.svg" "$OUT/logo-dark.svg"
# The site's design system, beside the page's fonts so that the service worker keeps it for offline starts.
cp "$HERE/ds.css" "$OUT/"
mkdir -p "$OUT/fonts"
cp "$HERE/home/fonts/"* "$OUT/fonts/"
cp "$PORT/resources/reg2_256.png" "$PORT/resources/reg2_512.png" "$OUT/icons/"

# The licences of what the site serves.
{
    echo "reG2's web build. The port's code (port/) is under the MIT licence; the game code comes from gta2_re."
    for f in "$PORT/LICENSE" "$P/licenses/"* "$OUT/vendor/"*.LICENSE "$OUT/fonts/"*.txt "$HERE/home/icons-licenses.txt"; do
        [ -f "$f" ] || continue
        printf '\n==== %s\n\n' "$(basename "$f")"
        cat "$f"
    done
} > "$OUT/licenses.txt"
rm -f "$OUT/vendor/"*.LICENSE "$OUT/fonts/"*.txt

# The service worker keeps this build's files for offline starts, under a version made from their contents.
VERSION=$(cd "$OUT" && find . -type f ! -name sw.js | sort | xargs cat | sha256sum | cut -c1-12)
FILES=$(cd "$OUT" && find . -type f ! -name sw.js | sort | sed 's|^\./||' | awk 'BEGIN{printf "["} {printf "%s\"%s\"", (NR>1?",":""), $0} END{printf "]"}')
sed -i "s|@VERSION@|$VERSION|; s|@FILES@|$FILES|" "$OUT/sw.js"
echo "site: $OUT (version $VERSION, $BUILD)"
