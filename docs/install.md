# Installing Hyprnotes

Hyprnotes is packaged for Arch Linux on x86_64. The package is called `hyprnotes` and installs `/usr/bin/hyprnotes`.

## What it needs

pacman pulls these in for you: `qt6-base`, `qt6-svg`, `qt6-wayland`, `md4c`, `sqlite`, `lua`, `libarchive`, `libglvnd`, `gcc-libs` and `glibc`.

The tray icon only shows up if something on your desktop hosts StatusNotifier icons. Waybar does that when its `tray` module is on. Without one, the app still works and you can reach it from the app drawer.

## Three ways to install

### Build the package yourself

This works from a fresh clone, no release needed.

```sh
packaging/build-package.sh
sudo pacman -U packaging/out/pkg/hyprnotes-*-x86_64.pkg.tar.zst
```

The build runs as your normal user and keeps its files in `packaging/out/`. You need `cmake` and `ninja` on top of the libraries above. To check what it produced, run `python3 packaging/tests/check_package.py`.

### A release package

Download the package and its `.sig` file from the [releases page](https://github.com/RolittapesReal/hyprnotes/releases), then verify the signature against the signing key's fingerprint (`425FEA9801A90AC5C5857A34A8607951FB5383CC`):

```sh
gpg --verify hyprnotes-0.1.0-1-x86_64.pkg.tar.zst.sig hyprnotes-0.1.0-1-x86_64.pkg.tar.zst
sudo pacman -U hyprnotes-0.1.0-1-x86_64.pkg.tar.zst
```

Upgrade the same way with a newer package. Remove it with `sudo pacman -R hyprnotes`.

### The bootstrap script

`packaging/bootstrap/install.sh` does the download and the checking for you:

```sh
HYPRNOTES_RELEASE_URL=https://github.com/RolittapesReal/hyprnotes/releases/download/v0.1.0 bash install.sh
```

The script has the release signing key's fingerprint built in (`425FEA9801A90AC5C5857A34A8607951FB5383CC`). It downloads from the matching release page.

The release directory has to contain `release.txt` (one line: `package=<file> sha256=<hex>`), the package, `<package>.sig` and `release-key.asc`. The script then does this, in order:

1. Checks that you are on Arch, x86_64, with the tools it needs.
2. Downloads everything into a private temp directory.
3. Verifies the signature in an isolated keyring against the fingerprint pinned in the script. The key file in the release is only trusted if it matches that fingerprint.
4. Checks the package name, version and architecture.
5. Runs `sudo pacman -U`, with pacman's normal confirmation prompt.

It never runs `pacman -Sy` or passes `--noconfirm`, it won't downgrade you, and it deletes only its own temp directory. If pacman doesn't trust the release key, you get pacman's own message and the script leaves pacman's policy alone.

Exit codes: 2 for a setup problem, 3 for a download or metadata problem, 4 for a failed verification, 5 for a package that doesn't validate, 6 when pacman fails or you cancel.

## What gets installed

| Path | What it is |
| --- | --- |
| `/usr/bin/hyprnotes` | the program |
| `/usr/share/applications/hyprnotes.desktop` | launcher, with a "New Note" action |
| `/usr/share/icons/hicolor/…/apps/hyprnotes.png`, `scalable/apps/hyprnotes.svg` | icons (16, 32, 48, 128 and 256 px) |
| `/usr/share/hyprnotes/` | example theme, autostart template, Hyprland snippets, example plugins and the native-plugin example |
| `/usr/include/hyprnotes/mod_api.h` | header for native plugins |
| `/usr/share/doc/hyprnotes/`, `/usr/share/licenses/hyprnotes/` | docs and the MIT license |

## Setting up your desktop

The package never touches your configuration. These are yours to do:

- **Waybar:** add `"tray"` to one of your module lists.
- **Autostart:** switch it on in Settings, or copy `/usr/share/hyprnotes/autostart/hyprnotes.desktop` into `~/.config/autostart/`.
- **Hyprland:** `source` or `dofile` one of the snippets in `/usr/share/hyprnotes/hyprland/`. Its README says which one fits your config.
- **Plugins:** try the examples in `/usr/share/hyprnotes/examples/plugins/` from Settings, Plugins. Read the warning in the dialog first. To build the native example, run `cmake -S /usr/share/hyprnotes/examples/mods/uppercase-selection -B /tmp/mod -DHN_MOD_INCLUDE=/usr/include`.

Your notes, settings and recovery files live outside the package, so removing or upgrading it leaves them alone. If Hyprnotes is running during an upgrade it keeps running the old version, so restart it to get the new one.
