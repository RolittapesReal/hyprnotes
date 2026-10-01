#!/usr/bin/env python3
"""Verify a built hyprnotes .pkg.tar.zst: files, modes, leaks, desktop entry, icon sizes, ELF linkage.
Usage: check_package.py [package]   (default: newest packaging/out/pkg/*.pkg.tar.zst)"""
import configparser, glob, io, os, re, struct, subprocess, sys, tarfile

root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
pkg = sys.argv[1] if len(sys.argv) > 1 else max(glob.glob(f"{root}/packaging/out/pkg/*.pkg.tar.zst"), key=os.path.getmtime)
fails = []
def check(ok, msg):
    print(("ok   " if ok else "FAIL ") + msg)
    if not ok: fails.append(msg)

tf = tarfile.open(fileobj=io.BytesIO(subprocess.run(["zstd", "-dc", pkg], check=True, capture_output=True).stdout))
mem = {m.name: m for m in tf.getmembers()}
data = lambda n: tf.extractfile(mem[n]).read()
files = {n for n, m in mem.items() if m.isfile() and not n.startswith(".")}

want = ["usr/bin/hyprnotes", "usr/share/applications/hyprnotes.desktop", "usr/share/icons/hicolor/scalable/apps/hyprnotes.svg",
        "usr/share/hyprnotes/themes/modernist-example.json", "usr/share/hyprnotes/autostart/hyprnotes.desktop",
        "usr/share/hyprnotes/hyprland/hyprnotes.conf", "usr/share/hyprnotes/hyprland/hyprnotes.lua",
        "usr/share/hyprnotes/examples/mods/uppercase-selection/uppercase_selection.c",
        "usr/include/hyprnotes/mod_api.h", "usr/share/hyprnotes/examples/plugins/word-count/plugin.json",
        "usr/share/doc/hyprnotes/plugins.md", "usr/share/doc/hyprnotes/plugin-api.md", "usr/share/doc/hyprnotes/README.md", "usr/share/doc/hyprnotes/install.md",
        "usr/share/licenses/hyprnotes/LICENSE"]
for w in want: check(w in files, f"contains {w}")

# modes: executable 0755, everything else non-executable and not group/world writable
check(mem["usr/bin/hyprnotes"].mode & 0o777 == 0o755, "usr/bin/hyprnotes mode 0755")
bad = [n for n in files if n != "usr/bin/hyprnotes" and mem[n].mode & 0o133]
check(not bad, f"non-binary files are 0644-style (bad: {bad})")
check(all(m.uname == "root" and m.gname == "root" for m in mem.values()), "all entries owned by root")
check(all(n == "usr" or n.startswith(("usr/", ".")) for n in mem), "everything under usr/")
allowed_exec = {"usr/bin/hyprnotes"}
check(not [n for n in files if n.startswith("usr/") and not n.startswith("usr/bin") and data(n)[:4] == b"\x7fELF"], "no stray ELF files")

# leaks
# docs may show example paths such as /home/me; everything else must be free of /home and build paths
leak = re.compile(rb"/home/|/run/media|packaging/out|/tmp/|/root/")
host = re.compile(re.escape(os.path.expanduser("~").encode()) + rb"|/run/media")
text_leaks = [n for n in files if n != "usr/bin/hyprnotes" and not n.endswith(".png")
              and (host if n.startswith("usr/share/doc/") else leak).search(data(n))]
check(not text_leaks, f"no /home or build paths in data files (hits: {text_leaks})")
ex = data("usr/bin/hyprnotes")
bin_leaks = sorted({m.group(0) for m in re.finditer(rb"(/home/[\x21-\x7e]*|/run/media[\x21-\x7e]*|packaging/out[\x21-\x7e]*)", ex)})
check(not bin_leaks, f"no /home or build path strings in the executable (hits: {bin_leaks[:5]})")
pkginfo = data(".PKGINFO").decode()
check("/home" not in pkginfo and "/run/media" not in pkginfo, ".PKGINFO has no host paths")

# desktop entry
for path, must in (("usr/share/applications/hyprnotes.desktop",
                    {"Exec": "hyprnotes --show-organizer", "Icon": "hyprnotes", "Terminal": "false", "StartupWMClass": "hyprnotes"}),):
    cp = configparser.RawConfigParser(strict=True); cp.optionxform = str
    cp.read_string(data(path).decode())
    de = cp["Desktop Entry"]
    for k, v in must.items(): check(de.get(k) == v, f"desktop {k}={v}")
    check("NewNote" in de.get("Actions", "").split(";"), "desktop Actions has NewNote")
    check(cp["Desktop Action NewNote"].get("Exec") == "hyprnotes --new-note", "NewNote Exec=hyprnotes --new-note")
au = data("usr/share/hyprnotes/autostart/hyprnotes.desktop").decode()
check("X-Hyprnotes-Managed=true" in au and "Exec=hyprnotes --background" in au, "autostart template marker and Exec")

# icons
for s in (16, 32, 48, 128, 256):
    n = f"usr/share/icons/hicolor/{s}x{s}/apps/hyprnotes.png"
    ok = n in files and data(n)[:8] == b"\x89PNG\r\n\x1a\n" and struct.unpack(">II", data(n)[16:24]) == (s, s)
    check(ok, f"icon {s}x{s} is a {s}x{s} PNG")
check(b"<svg" in data("usr/share/icons/hicolor/scalable/apps/hyprnotes.svg"), "scalable svg icon")

# package metadata and linkage
check(re.search(r"^pkgname = hyprnotes$", pkginfo, re.M) and re.search(r"^arch = x86_64$", pkginfo, re.M), "PKGINFO name/arch")
check(re.search(r"^license = MIT$", pkginfo, re.M), "PKGINFO license MIT")
deps = set(re.findall(r"^depend = (\S+)", pkginfo, re.M))
check({"qt6-base", "qt6-svg", "qt6-wayland", "md4c", "sqlite"} <= deps, f"PKGINFO depends {sorted(deps)}")
tmp = os.path.join(root, "packaging/out/check-hyprnotes"); open(tmp, "wb").write(ex)
needed = re.findall(r"Shared library: \[(\S+)\]", subprocess.run(["readelf", "-d", tmp], capture_output=True, text=True).stdout)
os.remove(tmp)
check("libQt6Widgets.so.6" in needed and "libmd4c.so.0" in needed, f"NEEDED {needed}")
check(not any("benchmark" in n or "experiments" in n for n in mem), "no benchmark/experiment files")

print(f"{len(fails)} failure(s)")
sys.exit(1 if fails else 0)
