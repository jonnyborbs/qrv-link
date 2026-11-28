#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Assemble the install page site (esptool-js) for a set of released firmware
# images. The page offers every version given here; <latest> is preselected.
#
#   install/build-site.sh <out-dir> <latest X.Y.Z> qrv-link-X.Y.Z-esp32s3.bin...
#
# The firmware is served from the same site as the page because browsers
# can't fetch GitHub release downloads directly (no CORS headers).
set -euo pipefail

if [ $# -lt 3 ]; then
  echo "usage: $0 <out-dir> <latest X.Y.Z> <qrv-link-X.Y.Z-esp32s3.bin>..." >&2
  exit 2
fi
out="$1"
latest="$2"
shift 2
here="$(cd "$(dirname "$0")" && pwd)"

mkdir -p "$out/firmware"
cp "$here/index.html" "$out/index.html"
cp "$here/case.png" "$out/case.png"
cp "$here/icon.png" "$out/icon.png"
touch "$out/.nojekyll"

# One "version file" line per image, newest first.
lines=""
for bin in "$@"; do
  file="$(basename "$bin")"
  version="$(echo "$file" | sed -nE 's/^qrv-link-([0-9]+\.[0-9]+\.[0-9]+)-esp32s3\.bin$/\1/p')"
  if [ -z "$version" ]; then
    echo "not a release image name: $file" >&2
    exit 1
  fi
  cp "$bin" "$out/firmware/$file"
  lines+="$version $file"$'\n'
done
sorted="$(printf '%s' "$lines" | sort -t. -k1,1nr -k2,2nr -k3,3nr)"
if ! printf '%s\n' "$sorted" | grep -q "^$latest "; then
  echo "latest version $latest is not among the images" >&2
  exit 1
fi

# Read by index.html. Versions before 0.0.4 advertise as "K.K. Link" (the
# project's earlier name) and the status LED arrived in 0.0.6, so describe
# each image by what it contains.
{
  echo "{"
  echo "  \"name\": \"QRV Link\","
  echo "  \"latest\": \"$latest\","
  echo "  \"versions\": ["
  sep=""
  while read -r version file; do
    name="QRV Link"
    if LC_ALL=C grep -aq "K.K. Link" "$out/firmware/$file"; then name="K.K. Link"; fi
    led=false
    if LC_ALL=C grep -aq "status_led" "$out/firmware/$file"; then led=true; fi
    printf '%s    { "version": "%s", "name": "%s", "led": %s, "firmware": "firmware/%s" }' \
      "$sep" "$version" "$name" "$led" "$file"
    sep=$',\n'
  done <<< "$sorted"
  echo
  echo "  ]"
  echo "}"
} > "$out/manifest.json"

echo "Site with $(printf '%s\n' "$sorted" | wc -l | tr -d ' ') version(s), latest $latest, written to $out"
