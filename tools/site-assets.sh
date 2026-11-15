#!/bin/sh
# Copies what the site shows from the repository into site/assets, so the
# screenshots and icons are kept in one place (docs/images, resources/icons, assets/).
# Run by .github/workflows/pages.yml before the build, and by tools/site-serve.sh.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
assets="$root/site/assets"
rm -rf "$assets/screenshots" "$assets/icons"
mkdir -p "$assets/screenshots" "$assets/icons"
cp "$root"/docs/images/* "$assets/screenshots/"
cp "$root"/resources/icons/konsoll/svg/*.svg "$assets/icons/"
cp "$root/assets/konsoll.png" "$assets/konsoll.png"
cp "$root/assets/qtlogo-64.png" "$assets/qtlogo.png"
