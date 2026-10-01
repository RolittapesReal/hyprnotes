#!/bin/sh
# Builds the Arch package unprivileged from the working tree. Output: packaging/out/*.pkg.tar.zst
# Never installs anything; makepkg runs with an isolated BUILDDIR/SRCDEST/PKGDEST under packaging/out.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
out=$root/packaging/out
ver=$(sed -n 's/^project(hyprnotes VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")
[ -n "$ver" ] || { echo "cannot read version from CMakeLists.txt" >&2; exit 1; }
rm -rf "$out"; mkdir -p "$out/work" "$out/build" "$out/src" "$out/pkg"
tarball=$out/work/hyprnotes-$ver.tar.gz
tar -C "$root" --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 \
    --transform "s,^\.,hyprnotes-$ver," \
    --exclude='./packaging/out' --exclude='*/__pycache__' \
    -czf "$tarball" ./CMakeLists.txt ./README.md ./LICENSE ./cmake ./src ./tests/unit ./examples ./docs ./packaging
sum=$(sha256sum "$tarball" | cut -d' ' -f1)
sed -e "s/^pkgver=.*/pkgver=$ver/" -e "s/^sha256sums=.*/sha256sums=('$sum')/" "$root/packaging/arch/PKGBUILD" > "$out/work/PKGBUILD"
cd "$out/work"
BUILDDIR=$out/build SRCDEST=$out/work PKGDEST=$out/pkg SRCPKGDEST=$out/pkg \
  makepkg -f --nocheck --noprogressbar "$@"
ls -1 "$out/pkg"/*.pkg.tar.zst
