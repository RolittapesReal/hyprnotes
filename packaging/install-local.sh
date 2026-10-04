#!/bin/sh
# Builds the package from this working tree and installs it (asks for sudo). Restarts a running hyprnotes.
set -eu
cd "$(dirname "$0")/.."
packaging/build-package.sh
pkg=$(ls -t packaging/out/pkg/hyprnotes-*.pkg.tar.zst | head -n 1)
pkill -x hyprnotes || true
sudo pacman -U "$pkg"
