#!/usr/bin/env bash
# Build the SD-card release zip: dist-zip/Euclidier-<version>.zip, unpacking to
#   AddOns/Euclidier/   euclidier + NSMODULE.json + shadow_page.conf
# - the same three files scripts/deploy.sh copies. Unzip it onto the SD card
# root. The preset bank (euclidierBANK.bin) is deliberately left out: the
# binary creates it next to itself on first run, and shipping one would
# overwrite saved presets. The binary isn't committed, so this runs
# build/build.sh (Docker, armhf under QEMU) first unless bin/euclidier exists.
# .github/workflows/release.yml runs this for every published release.
set -euo pipefail
cd "${PKG_ROOT:-$(dirname "$0")/..}"
VER="${1:-$(git describe --tags --always)}"
[ -f bin/euclidier ] || build/build.sh
OUT="$PWD/dist-zip"
STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
A="$STAGE/AddOns/Euclidier"
mkdir -p "$A" "$OUT"
cp bin/euclidier addon/NSMODULE.json addon/shadow_page.conf "$A/"
chmod 0755 "$A/euclidier"
rm -f "$OUT/Euclidier-$VER.zip"
python3 -c "import shutil,sys; shutil.make_archive(sys.argv[1], 'zip', sys.argv[2], 'AddOns')" "$OUT/Euclidier-$VER" "$STAGE"
python3 -m zipfile -l "$OUT/Euclidier-$VER.zip"
