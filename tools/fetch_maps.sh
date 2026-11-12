#!/bin/sh
# Download the OpenFront map sources used by tools/mapconv.py into
# vendor/openfront-maps. They are licensed CC BY-SA 4.0 by OpenFront.
set -e
cd "$(dirname "$0")/.."
MAPS="world europe africa asia northamerica southamerica oceania mena britannia japan blacksea italia australia pangaea mars"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git clone --quiet --depth 1 --filter=blob:none --sparse https://github.com/openfrontio/OpenFrontIO.git "$tmp/of"
paths=""
for m in $MAPS; do paths="$paths resources/maps/$m"; done
(cd "$tmp/of" && git sparse-checkout set $paths)
for m in $MAPS; do
    mkdir -p "vendor/openfront-maps/$m"
    cp "$tmp/of/resources/maps/$m/manifest.json" "$tmp/of/resources/maps/$m/map.bin" "vendor/openfront-maps/$m/"
done
cp "$tmp/of/LICENSE-ASSETS" vendor/openfront-maps/
echo "OpenFront maps are in vendor/openfront-maps (CC BY-SA 4.0)"
