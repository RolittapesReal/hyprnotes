#!/bin/bash
# Tests packaging/bootstrap/install.sh against a local file:// release signed by throwaway keys.
set -u
here=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
export GNUPGHOME=$W/gh; mkdir -m 700 "$GNUPGHOME"
mkkey() { gpg --batch --quiet --passphrase '' --quick-generate-key "$1" default default never 2>/dev/null
          gpg --with-colons --list-keys "$1" | awk -F: '/^fpr/{print $10; exit}'; }
FPR_A=$(mkkey "test-a@example.invalid"); FPR_B=$(mkkey "test-b@example.invalid")

# Script under test with the throwaway fingerprint pinned; the untouched script is used for the placeholder test.
sed "s/^PINNED_FPR=.*/PINNED_FPR=\"$FPR_A\"/" "$here/../bootstrap/install.sh" > "$W/install.sh"

cat > "$W/pacman" <<'STUB'
#!/bin/sh
echo "$@" >> "$STUB_LOG"
case "$1" in
  -Q) [ -n "${STUB_INSTALLED:-}" ] && { echo "hyprnotes $STUB_INSTALLED"; exit 0; }; exit 1 ;;
  -U) exit "${STUB_U_EXIT:-0}" ;;
esac
exit 99
STUB
chmod +x "$W/pacman"

mkpkg() { # dir arch ver -> file name
  local d=$W/p$$-$2-$3; rm -rf "$d"; mkdir -p "$d"
  printf 'pkgname = hyprnotes\npkgver = %s\narch = %s\n' "$3" "$2" > "$d/.PKGINFO"
  bsdtar --zstd -cf "$W/hyprnotes-$3-$2.pkg.tar.zst" -C "$d" .PKGINFO
  echo "hyprnotes-$3-$2.pkg.tar.zst"
}
release() { # name signer-email arch ver
  local r=$W/$1; rm -rf "$r"; mkdir -p "$r"
  local f; f=$(mkpkg x "${3:-x86_64}" "${4:-0.1.0-1}"); mv "$W/$f" "$r/$f"
  gpg --batch --yes --quiet --local-user "$2" --detach-sign -o "$r/$f.sig" "$r/$f"
  gpg --armor --export "test-a@example.invalid" "test-b@example.invalid" > "$r/release-key.asc"
  echo "package=$f sha256=$(sha256sum "$r/$f" | cut -d' ' -f1)" > "$r/release.txt"
}

pass=0; fail=0
run() { # name expected-exit script url [env...]
  local name=$1 want=$2 script=$3 url=$4; shift 4
  mkdir -p "$W/tmpdir"; : > "$W/stub.log"
  env TMPDIR="$W/tmpdir" HYPRNOTES_RELEASE_URL="$url" HYPRNOTES_PACMAN="$W/pacman" STUB_LOG="$W/stub.log" "$@" \
    bash "$script" > "$W/out.txt" 2>&1
  local got=$?
  local left; left=$(ls -A "$W/tmpdir" | wc -l)
  if [ "$got" = "$want" ] && [ "$left" = 0 ]; then pass=$((pass+1)); echo "ok   $name (exit $got, temp cleaned)"
  else fail=$((fail+1)); echo "FAIL $name: exit $got want $want, leftover temp entries $left"; sed 's/^/     | /' "$W/out.txt"; fi
}
installed_called() { grep -q '^-U ' "$W/stub.log"; }
expect_U() { if installed_called; then echo "     pacman -U reached: $(grep '^-U' "$W/stub.log" | sed "s|$W/tmpdir/[^/]*/||")"; else fail=$((fail+1)); echo "FAIL pacman -U not reached"; fi; }
expect_noU() { if installed_called; then fail=$((fail+1)); echo "FAIL pacman -U reached but must not"; fi; }

release good test-a@example.invalid
run valid-signature-accepted 0 "$W/install.sh" "file://$W/good"
expect_U
grep -q -- '--noconfirm' "$W/stub.log" && { fail=$((fail+1)); echo "FAIL --noconfirm used"; }
grep -Eq -- '-S' "$W/stub.log" && { fail=$((fail+1)); echo "FAIL -S used"; }

run already-installed-same-version 0 "$W/install.sh" "file://$W/good" STUB_INSTALLED=0.1.0-1; expect_noU
run installed-newer-no-downgrade 5 "$W/install.sh" "file://$W/good" STUB_INSTALLED=0.2.0-1; expect_noU
run upgrade-from-older 0 "$W/install.sh" "file://$W/good" STUB_INSTALLED=0.0.9-1; expect_U

release tampered test-a@example.invalid; printf 'x' >> "$W/tampered/hyprnotes-0.1.0-1-x86_64.pkg.tar.zst"
run altered-package-rejected 4 "$W/install.sh" "file://$W/tampered"; expect_noU
# altered package with a matching (attacker-recomputed) sha256: signature must still stop it
echo "package=hyprnotes-0.1.0-1-x86_64.pkg.tar.zst sha256=$(sha256sum "$W/tampered/hyprnotes-0.1.0-1-x86_64.pkg.tar.zst" | cut -d' ' -f1)" > "$W/tampered/release.txt"
run altered-package-with-fixed-checksum-rejected 4 "$W/install.sh" "file://$W/tampered"; expect_noU

release wrongsigner test-b@example.invalid
run wrong-signer-rejected 4 "$W/install.sh" "file://$W/wrongsigner"; expect_noU

release wrongarch test-a@example.invalid aarch64
# release.txt must still pass the filename filter, so the aarch64 package is renamed to an x86_64 name (re-signed)
r=$W/wrongarch; mv "$r"/hyprnotes-0.1.0-1-aarch64.pkg.tar.zst "$r"/hyprnotes-0.1.0-1-x86_64.pkg.tar.zst; rm "$r"/*.sig
gpg --batch --yes --quiet --local-user test-a@example.invalid --detach-sign -o "$r/hyprnotes-0.1.0-1-x86_64.pkg.tar.zst.sig" "$r/hyprnotes-0.1.0-1-x86_64.pkg.tar.zst"
echo "package=hyprnotes-0.1.0-1-x86_64.pkg.tar.zst" > "$r/release.txt"
run wrong-arch-package-rejected 5 "$W/install.sh" "file://$W/wrongarch"; expect_noU

release aarch test-a@example.invalid aarch64
run wrong-arch-filename-rejected 3 "$W/install.sh" "file://$W/aarch"; expect_noU

release nometa test-a@example.invalid; rm "$W/nometa/release.txt"
run missing-metadata-rejected 3 "$W/install.sh" "file://$W/nometa"; expect_noU
release nosig test-a@example.invalid; rm "$W/nosig"/*.sig
run missing-signature-rejected 3 "$W/install.sh" "file://$W/nosig"; expect_noU
run unset-url-rejected 2 "$W/install.sh" ""; expect_noU
run http-url-rejected 2 "$W/install.sh" "http://example.invalid/r"; expect_noU
sed '9s/.*/PINNED_FPR="REPLACE_WITH_RELEASE_SIGNING_FINGERPRINT"/' "$here/../bootstrap/install.sh" > "$W/placeholder.sh"
run placeholder-fingerprint-refused 2 "$W/placeholder.sh" "file://$W/good"; expect_noU

run cancel-at-pacman-nonzero 6 "$W/install.sh" "file://$W/good" STUB_U_EXIT=1
expect_U

echo "passed=$pass failed=$fail"
[ "$fail" = 0 ]
