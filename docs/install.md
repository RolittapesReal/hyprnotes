# Installing Hyprnotes

Supported: Arch Linux, x86_64. The package is `hyprnotes`; the executable is `/usr/bin/hyprnotes`.

## Runtime dependencies

`qt6-base`, `qt6-svg`, `qt6-wayland`, `md4c`, `sqlite`, `libglvnd`, `gcc-libs`, `glibc` (pacman installs them). A tray icon needs a StatusNotifier host, e.g. Waybar with the `tray` module enabled; without one the app stays reachable from the app drawer.

## Option 1: release package

Download the package and its `.sig` from the release you trust, then verify with the release signing key (fingerprint published with the release):

```sh
gpg --verify hyprnotes-<ver>-x86_64.pkg.tar.zst.sig hyprnotes-<ver>-x86_64.pkg.tar.zst
sudo pacman -U hyprnotes-<ver>-x86_64.pkg.tar.zst
```

Upgrade the same way with the newer package. Remove with `sudo pacman -R hyprnotes`.

## Option 2: bootstrap script

```sh
HYPRNOTES_RELEASE_URL=https://<release origin>/<dir> bash install.sh
```

The release directory must hold `release.txt` (`package=<file> sha256=<hex>`), the package, `<package>.sig` and `release-key.asc`. The script checks Arch/x86_64/tools, downloads into a private temp directory, verifies the detached signature in an isolated keyring against the fingerprint pinned inside the script (a placeholder fingerprint is refused), checks package name, version and architecture, then runs `sudo pacman -U` with the normal confirmation. It never runs `pacman -Sy` or `--noconfirm`, does not downgrade, and removes only its own temp directory. If the release key is not trusted by pacman, pacman's own message is shown; the script does not weaken pacman policy. Exit codes: 2 environment/config, 3 download/metadata, 4 verification, 5 package validation, 6 pacman failure or cancel.

## Option 3: build from source

```sh
packaging/build-package.sh                # unprivileged; uses packaging/out/ as its work area
sudo pacman -U packaging/out/pkg/hyprnotes-*-x86_64.pkg.tar.zst
```

Build dependencies: `cmake`, `ninja`, plus the runtime libraries above. Check the result with `python3 packaging/tests/check_package.py`.

## What is installed

| Path | Content |
| --- | --- |
| `/usr/bin/hyprnotes` | executable |
| `/usr/share/applications/hyprnotes.desktop` | launcher with a "New Note" action |
| `/usr/share/icons/hicolor/{16,32,48,128,256}x*/apps/hyprnotes.png`, `scalable/apps/hyprnotes.svg` | icon |
| `/usr/share/hyprnotes/` | example theme, autostart template, Hyprland snippets, example mod |
| `/usr/include/hyprnotes/mod_api.h` | mod SDK header |
| `/usr/share/doc/hyprnotes/`, `/usr/share/licenses/hyprnotes/` | docs, MIT license |

## Desktop integration (per user, never done by the package)

- Waybar: add `"tray"` to a modules list.
- Autostart: toggle it in Settings, or copy `/usr/share/hyprnotes/autostart/hyprnotes.desktop` to `~/.config/autostart/`.
- Hyprland: `source` or `dofile` a snippet from `/usr/share/hyprnotes/hyprland/` (see its README).
- Building the example mod: `cmake -S /usr/share/hyprnotes/examples/mods/uppercase-selection -B /tmp/mod -DHN_MOD_INCLUDE=/usr/include`.

Notes, config and recovery data are outside the package and survive removal. Updating while Hyprnotes runs does not kill it; restart it to use the new version.
