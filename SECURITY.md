# Security policy

## Reporting a problem

Report security problems privately through the repository's Security tab: choose "Report a vulnerability". Include the version from `hyprnotes --version`, the steps you took and what happened. A short proof of concept is useful.

One person maintains Hyprnotes. I read every report and reply as soon as I can, though I cannot promise a response time.

## Supported versions

Security fixes are made for the latest release. Upgrade from an older release to receive them.

## What counts

- A plugin escaping the Lua sandbox, or using a permission it was not granted.
- A way to get a plugin installed or enabled without the permission dialog.
- A flaw in how the Browse tab downloads, verifies or installs plugins, or in the bootstrap installer's signature checks.
- Anything that reads, changes or deletes notes outside what the user asked for.

## What doesn't

- A plugin that does what its approved permissions allow. The app warns about this, and the project does not review third-party plugins.
- Native plugins. They run with full access to your account and are not sandboxed by design.
- Problems that need an attacker who can already run code as you or edit your files.

## Verifying a download

Release packages are signed. Check the signing key fingerprint listed in [docs/install.md](docs/install.md) before trusting a package. The SHA-256 check in the Browse tab confirms that a download matches the registry index; it does not show whether the plugin is safe.
