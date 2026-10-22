# Installing Hyprnotes

Hyprnotes is packaged for Arch Linux on x86_64. The package installs `/usr/bin/hyprnotes`. It requires a Wayland session and is built and tested with Hyprland in mind.

## Before you start

Check that you are on Arch (or something that uses pacman) and on x86_64:

```sh
uname -m          # x86_64
pacman --version  # prints the pacman banner
```

pacman pulls in the runtime libraries for you: `qt6-base`, `qt6-svg`, `qt6-wayland`, `md4c`, `sqlite` (3.35 or newer; older versions make search report "unavailable" instead of returning nothing), `lua`, `libarchive`, `libglvnd`, `gcc-libs` and `glibc`. Building the package yourself also needs `git`, `base-devel`, `cmake` and `ninja`:

```sh
sudo pacman -S --needed git base-devel cmake ninja
```

The tray icon requires a desktop StatusNotifier host. Waybar provides one when its `tray` module is enabled. Hyprnotes still works without a tray host; you can open it from the app drawer.

## Three ways to install

Choose one of these methods. Each installs the same pacman package.

### 1. A release package

The commands below use version 0.2.2. While it remains unpublished, install the latest available release or build from source.

Use this route if you already have the signed package files.

1. Download these three files from the [releases page](https://github.com/RolittapesReal/hyprnotes/releases) into one folder: the `hyprnotes-<version>-1-x86_64.pkg.tar.zst` package, its `.sig` file and `release-key.asc`.
2. Import the signing key and look at its fingerprint. It must read `1B42F4BE4E8C0722BE5D6EBD40A7D03B60FA39B5`. Stop if it doesn't:

   ```sh
   gpg --import release-key.asc
   gpg --fingerprint 1B42F4BE4E8C0722BE5D6EBD40A7D03B60FA39B5
   ```

   If the second command says "No public key", the file you imported isn't ours.
3. Verify the package against the signature. You should see `Good signature` and the same fingerprint:

   ```sh
   gpg --verify hyprnotes-0.2.2-1-x86_64.pkg.tar.zst.sig hyprnotes-0.2.2-1-x86_64.pkg.tar.zst
   ```

   gpg may also warn that the key is not certified with a trusted signature. That is normal for a key you haven't signed yourself; the fingerprint match is the check that counts.
4. Install it. pacman shows what it will do and asks before it continues:

   ```sh
   sudo pacman -U hyprnotes-0.2.2-1-x86_64.pkg.tar.zst
   ```

### 2. The bootstrap script

`packaging/bootstrap/install.sh` downloads and verifies a release before installing it. Read the script, then run it with the release you want:

```sh
git clone https://github.com/RolittapesReal/hyprnotes.git
cd hyprnotes
HYPRNOTES_RELEASE_URL=https://github.com/RolittapesReal/hyprnotes/releases/download/v0.2.2 bash packaging/bootstrap/install.sh
```

The script has the release signing key's fingerprint built in (`1B42F4BE4E8C0722BE5D6EBD40A7D03B60FA39B5`). It downloads from the matching release page.

The release directory has to contain `release.txt` (one line: `package=<file> sha256=<hex>`), the package, `<package>.sig` and `release-key.asc`. The script then does this, in order:

1. Checks that you are on Arch, x86_64, with the tools it needs.
2. Downloads everything into a private temp directory.
3. Verifies the signature in an isolated keyring against the fingerprint pinned in the script. The key file in the release is only trusted if it matches that fingerprint.
4. Checks the package name, version and architecture.
5. Runs `sudo pacman -U`, with pacman's normal confirmation prompt.

It never runs `pacman -Sy` or passes `--noconfirm`, it won't downgrade you, and it deletes only its own temp directory. If pacman doesn't trust the release key, you get pacman's own message and the script leaves pacman's policy alone.

Exit codes: 2 for a setup problem, 3 for a download or metadata problem, 4 for a failed verification, 5 for a package that doesn't validate, 6 when pacman fails or you cancel.

### 3. Build the package yourself

Build from a fresh clone when you do not have a release package or need an unreleased change.

```sh
git clone https://github.com/RolittapesReal/hyprnotes.git
cd hyprnotes
packaging/build-package.sh
```

The build runs as your normal user and keeps its files in `packaging/out/`; nothing is installed yet. The package ends up in `packaging/out/pkg/`. To check what it produced, run `python3 packaging/tests/check_package.py`. Then install it:

```sh
sudo pacman -U packaging/out/pkg/hyprnotes-*-x86_64.pkg.tar.zst
```

If you already have a clone and want to rebuild and reinstall in one go, `packaging/install-local.sh` builds, closes a running Hyprnotes and runs the same `pacman -U` (it still asks for sudo and for confirmation).

## First run

Start it from a terminal on first launch to see any messages:

```sh
hyprnotes
```

It asks where to keep your notes (the default is `~/Notes/Hyprnotes`) and then opens the organizer. After that:

- `hyprnotes --show-organizer` brings the organizer up, and `hyprnotes --new-note` opens a fresh sticky note. Running it again talks to the instance that is already open.
- The app also appears in your app drawer as Hyprnotes, with a "New Note" action.
- Settings are in `~/.config/hyprnotes/`, and your notes stay plain `.md` files in the folder you chose.

## Updating and removing

Install an updated package using the same method as the first one. If Hyprnotes is open during the upgrade, it keeps running the old version; close and reopen it to use the update.

Remove it with:

```sh
sudo pacman -R hyprnotes
```

This leaves notes, settings, recovery files and installed plugins in place. To remove those too, delete your notes folder and `~/.config/hyprnotes/`, `~/.local/share/hyprnotes/`, `~/.local/state/hyprnotes/` and `~/.cache/hyprnotes/`. Check the notes folder before deleting it.

## If something goes wrong

- **`gpg: Can't check signature: No public key`:** you skipped the key import in step 2 of the release package route.
- **`pacman: invalid or corrupted package`:** the download is damaged. Download it again and compare the sha256 with the one in `release.txt`.
- **`ninja: command not found` while building:** install `ninja` and `cmake` (see "Before you start").
- **No tray icon:** turn on Waybar's `tray` module. The app itself keeps working without it.
- **The window opens but looks wrong or is missing:** run `hyprnotes` from a terminal and read what it prints, then open an issue with that output.

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

The package does not edit your configuration. Set up any of these yourself:

- **Waybar:** add `"tray"` to one of your module lists.
- **Autostart:** switch it on in Settings, or copy `/usr/share/hyprnotes/autostart/hyprnotes.desktop` into `~/.config/autostart/`.
- **Hyprland:** `source` or `dofile` one of the snippets in `/usr/share/hyprnotes/hyprland/`. Its README says which one fits your config.
- **Plugins:** try the examples in `/usr/share/hyprnotes/examples/plugins/` from Settings, Plugins. Read the warning in the dialog first. To build the native example, run `cmake -S /usr/share/hyprnotes/examples/mods/uppercase-selection -B /tmp/mod -DHN_MOD_INCLUDE=/usr/include`.

Notes, settings and recovery files live outside the package, so upgrades and removal leave them in place. If Hyprnotes was open during an upgrade, restart it to use the new version.
