#!/bin/bash
# Download the latest pre-built release (miner + .so libs) from GitHub Releases into ./build/.
# Requires a published release with a `build.tar.gz` (or `build-aarch64.tar.gz`) asset.
set -eu
REPO="${REPO:-arabel1a/ascend_prl}"
cd "$(dirname "$0")/.."
api="https://api.github.com/repos/$REPO/releases/latest"
url=$(curl -fsSL "$api" | grep -oE '"browser_download_url": *"[^"]*build[^"]*\.tar\.gz"' | head -1 | cut -d'"' -f4)
[ -n "$url" ] || { echo "no build tarball in the latest release of $REPO — build from source (see README)"; exit 1; }
echo "downloading $url"
mkdir -p build && curl -fL "$url" | tar xz -C build --strip-components=1
echo "done -> ./build/ ; run with scripts/launch.sh"
