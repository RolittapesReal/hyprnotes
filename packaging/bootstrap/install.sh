#!/bin/bash
# Hyprnotes bootstrap installer (Arch Linux, x86_64). Runs as your user; only `pacman -U` is elevated (via sudo).
# Usage: HYPRNOTES_RELEASE_URL=https://<origin>/<release dir> ./install.sh
# The release directory must contain: release.txt (package=<file>[ sha256=<hex>]), <file>, <file>.sig, release-key.asc.
# Authenticity comes from the pinned fingerprint below, never from release.txt.
set -euo pipefail

# Release owner: replace with the 40-hex primary fingerprint of the release signing key before publishing.
PINNED_FPR="REPLACE_WITH_RELEASE_SIGNING_FINGERPRINT"
PKGNAME=hyprnotes
ARCH=x86_64

# Test-only override of the pacman binary (called directly, no sudo). Never set this in normal use.
PACMAN=${HYPRNOTES_PACMAN:-pacman}

die() { code=$1; shift; echo "hyprnotes-install: $*" >&2; exit "$code"; }

[ "$(id -u)" -ne 0 ] || die 2 "run as your normal user, not root (sudo is used only for pacman -U)"
[[ $PINNED_FPR =~ ^[0-9A-Fa-f]{40}$ ]] || die 2 "this installer has no pinned release-signing fingerprint (placeholder); refusing to install unverified software"
PINNED_FPR=${PINNED_FPR^^}
url=${HYPRNOTES_RELEASE_URL:-}
[ -n "$url" ] || die 2 "HYPRNOTES_RELEASE_URL is not set; it must point at the release directory"
[[ $url =~ ^(https|file):// ]] || die 2 "HYPRNOTES_RELEASE_URL must be https:// (or file://)"
url=${url%/}

. /etc/os-release 2>/dev/null || true
[ "${ID:-}" = arch ] || die 2 "supported on Arch Linux only"
[ "$(uname -m)" = "$ARCH" ] || die 2 "supported architecture is $ARCH, this is $(uname -m)"
for t in curl gpg bsdtar sha256sum vercmp mktemp "$PACMAN"; do
  command -v "$t" >/dev/null || die 2 "required tool not found: $t"
done
[ -n "${HYPRNOTES_PACMAN:-}" ] || command -v sudo >/dev/null || die 2 "required tool not found: sudo"

tmp=$(mktemp -d "${TMPDIR:-/tmp}/hyprnotes-install.XXXXXXXX")
trap 'rm -rf -- "$tmp"' EXIT
chmod 700 "$tmp"

fetch() { curl --fail --silent --show-error --location --proto '=https,file' --proto-redir '=https' \
               --connect-timeout 15 --max-time 300 --max-filesize 536870912 -o "$2" "$url/$1" \
          || die 3 "download failed: $url/$1"; }

fetch release.txt "$tmp/release.txt"
pkg=$(sed -n 's/^package=\([^ ]*\).*/\1/p' "$tmp/release.txt" | head -n1)
sum=$(sed -n 's/.* sha256=\([0-9a-fA-F]\{64\}\).*/\1/p' "$tmp/release.txt" | head -n1)
[ -n "$pkg" ] || die 3 "release.txt has no package= entry"
[[ $pkg =~ ^hyprnotes-[0-9][A-Za-z0-9._+]*-[0-9]+-x86_64\.pkg\.tar\.zst$ ]] || die 3 "release.txt names an unacceptable package file: $pkg"

fetch "$pkg" "$tmp/$pkg"
fetch "$pkg.sig" "$tmp/$pkg.sig"
fetch release-key.asc "$tmp/release-key.asc"
[ -z "$sum" ] || [ "$(sha256sum "$tmp/$pkg" | cut -d' ' -f1)" = "${sum,,}" ] || die 4 "sha256 mismatch for $pkg"

export GNUPGHOME=$tmp/gnupg
mkdir -m 700 "$GNUPGHOME"
gpg --batch --quiet --import "$tmp/release-key.asc" 2>/dev/null || die 4 "cannot import release key"
status=$(gpg --batch --status-fd 1 --verify "$tmp/$pkg.sig" "$tmp/$pkg" 2>/dev/null) || die 4 "signature verification failed"
grep -q '^\[GNUPG:\] GOODSIG ' <<<"$status" || die 4 "no good signature"
grep -Eq '^\[GNUPG:\] (BADSIG|EXPKEYSIG|EXPSIG|REVKEYSIG|ERRSIG)' <<<"$status" && die 4 "bad, expired or revoked signature"
primary=$(awk '$2=="VALIDSIG"{print toupper($NF)}' <<<"$status")
[ "$primary" = "$PINNED_FPR" ] || die 4 "package is not signed by the pinned release key (signer: ${primary:-unknown})"

info=$(bsdtar -xOf "$tmp/$pkg" .PKGINFO 2>/dev/null) || die 5 "cannot read .PKGINFO from package"
field() { sed -n "s/^$1 = //p" <<<"$info" | head -n1; }
[ "$(field pkgname)" = "$PKGNAME" ] || die 5 "unexpected package name: $(field pkgname)"
[ "$(field arch)" = "$ARCH" ] || die 5 "unexpected package architecture: $(field arch)"
ver=$(field pkgver)
[ -n "$ver" ] && [ "$pkg" = "$PKGNAME-$ver-$ARCH.pkg.tar.zst" ] || die 5 "package version $ver does not match file name $pkg"

if installed=$("$PACMAN" -Q "$PKGNAME" 2>/dev/null); then
  cur=${installed#* }
  case $(vercmp "$cur" "$ver") in
    0) echo "hyprnotes $cur is already installed."; exit 0 ;;
    1) die 5 "installed version $cur is newer than $ver; not downgrading" ;;
  esac
fi

echo "Verified $pkg (signed by $PINNED_FPR). Installing with pacman; confirm when asked."
if [ -n "${HYPRNOTES_PACMAN:-}" ]; then
  echo "hyprnotes-install: TEST MODE, using $PACMAN without sudo" >&2
  "$PACMAN" -U -- "$tmp/$pkg" || die 6 "pacman did not complete the installation"
else
  sudo pacman -U -- "$tmp/$pkg" || die 6 "pacman did not complete the installation"
fi
echo "Done. Run: hyprnotes"
