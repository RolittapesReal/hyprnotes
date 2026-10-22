#!/usr/bin/env python3
import json
import os
import re
import shlex
import shutil
import socket
import socketserver
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

from driver_protocol import Driver, scratch_env


def rig_json(rig, *args):
    return json.loads(subprocess.check_output([rig, "hyprctl", "-j", *args], text=True))


def clients(rig):
    return [item for item in rig_json(rig, "clients") if item.get("class") == "hyprnotes"]


def by_role(rig, role):
    return [item for item in clients(rig) if item.get("initialTitle", "").startswith(f"hyprnotes-{role}:")]


def active_window(rig):
    return rig_json(rig, "activewindow")


def lua_dispatch(rig, command):
    result = subprocess.run([rig, "hyprctl", "dispatch", command], text=True, capture_output=True)
    if result.returncode != 0:
        raise AssertionError(f"nested Lua dispatch failed: {command}: {result.stdout} {result.stderr}")
    return result.stdout.strip()


def wait_role(rig, role, count=1):
    def matching():
        found = by_role(rig, role)
        return found if len(found) == count else None
    return wait_for(matching, f"{count} {role} client(s)")


def wait_client_address(rig, address):
    return wait_for(lambda: next((c for c in clients(rig) if c.get("address") == address), None),
                    f"client {address}")


def client_for_note(rig, driver, rel):
    state = driver.request("state")
    window = next((w for w in state.get("windows", []) if w.get("rel") == rel), None)
    assert window, f"no app window for {rel}: {state}"
    token = window["token"]
    return wait_for(lambda: next((c for c in clients(rig) if c.get("initialTitle", "").endswith(":" + token)), None),
                    f"Hyprland client for note {rel}")


def output_monitor(rig, name):
    return next(m for m in rig_json(rig, "monitors") if m.get("name") == name)


def write_nested_config(text):
    """Replace the pinned nested config through the lifecycle controller (digest moves with it)."""
    session = os.environ["HN_LIVE_SESSION_DIR"]
    controller = Path(__file__).resolve().parent / "lifecycle.py"
    subprocess.run([sys.executable, str(controller), "reconfig", session], input=text.encode(),
                   check=True, stdout=subprocess.DEVNULL)


def set_nested_monitor_config(rig_dir, mode, scale, output=None, position="auto"):
    config = Path(os.environ["HN_LIVE_SESSION_DIR"], "hyprland.lua")
    source = config.read_text()
    if output is not None:
        lines = [line for line in source.splitlines() if "-- hn-task6-monitor" not in line]
        lines.append(f"hl.monitor({{ output = '{output}', mode = '{mode}', position = '{position}', scale = {scale} }}) -- hn-task6-monitor")
        write_nested_config("\n".join(lines) + "\n")
        return
    pattern = re.compile(r'(hl\.monitor\(\{ output = "", mode = ")[^"]+(", position = "auto", scale = ")[^"]+(" \}\))')
    updated, count = pattern.subn(rf"\g<1>{mode}\g<2>{scale}\g<3>", source, count=1)
    assert count == 1, f"cannot find nested fallback monitor rule in {config}"
    write_nested_config(updated)


def assert_client_inside_monitor(client, monitor):
    x, y = client["at"]
    w, h = client["size"]
    left, top = monitor.get("x", 0), monitor.get("y", 0)
    scale = monitor.get("scale", 1) or 1
    logical_width, logical_height = monitor["width"] / scale, monitor["height"] / scale
    assert x >= left and y >= top and x + w <= left + logical_width + 3 and y + h <= top + logical_height + 3, \
        f"window outside monitor bounds: client={client}, monitor={monitor}"


def check_focus_and_stacking(rig, driver_path, scratch):
    app = LiveApp(driver_path, scratch)
    driver = app.start()
    rels = []
    try:
        for _ in range(2):
            created = driver.request("new")
            assert created.get("ok"), created
            rels.append(created["rel"])
            wait_role(rig, "sticky", len(rels))
            client = client_for_note(rig, driver, created["rel"])
            active = active_window(rig)
            assert active.get("address") == client["address"], f"new sticky did not receive focus: {active}, sticky={client}"
        first, second = (client_for_note(rig, driver, rel) for rel in rels)
        assert first.get("floating") is True and first.get("pinned") is False, f"sticky stacking mode must be floating, not pinned: {first}"

        assert driver.request("peer").get("ok") is True
        peer = wait_for(lambda: next((c for c in clients(rig) if c.get("initialTitle") == "task6-peer"), None), "tiled focus peer")
        focused_peer = active_window(rig)
        assert focused_peer.get("address") == peer["address"], f"peer did not receive focus: {focused_peer}"
        assert peer.get("floating") is False, f"focus peer should provide tiled layer: {peer}"
        first = wait_client_address(rig, first["address"])
        assert first.get("floating") is True and first.get("pinned") is False and first.get("visible") is True, \
            f"sticky should remain visible in Hyprland floating layer without all-workspace pinning: {first}"
        lua_dispatch(rig, f"hl.dsp.window.alter_zorder({{window='address:{first['address']}',mode='top'}})")
        assert active_window(rig).get("address") == peer["address"], "z-order change unexpectedly stole focus"
        first = wait_client_address(rig, first["address"])
        assert first.get("floating") is True and first.get("pinned") is False, f"z-order operation changed sticky mode: {first}"

        assert driver.request("close-peer").get("ok") is True
        wait_for(lambda: active_window(rig).get("address") == second["address"], "focus return to prior sticky")
        assert driver.request("close", rel=rels[1]).get("ok") is True
        wait_for(lambda: active_window(rig).get("address") == first["address"], "focus return after closing newest sticky")
        surviving = wait_client_address(rig, first["address"])
        assert surviving.get("visible") is True and surviving.get("floating") is True and surviving.get("pinned") is False, surviving
        print("PASS focus_and_stacking: new sticky focus, peer focus, floating/unpinned layer, z-order, close-focus return")
    finally:
        app.stop()


def check_workspaces(rig, driver_path, scratch):
    app = LiveApp(driver_path, scratch)
    driver = app.start()
    rel = ""
    peer_address = None
    try:
        rel = driver.request("new")["rel"]
        wait_role(rig, "sticky")
        client = client_for_note(rig, driver, rel)
        assert driver.request("workspace-mode", rel=rel, mode="this").get("ok") is True
        lua_dispatch(rig, f"hl.dsp.window.move({{window='address:{client['address']}',workspace=1,follow=false}})")
        original_ws = next(c for c in clients(rig) if c["address"] == client["address"])["workspace"]["id"]
        lua_dispatch(rig, "hl.dsp.focus({workspace='2'})")
        wait_for(lambda: rig_json(rig, "activeworkspace").get("id") == 2, "workspace 2 activation")
        client = wait_client_address(rig, client["address"])
        assert client["workspace"]["id"] == original_ws and client.get("pinned") is False, \
            f"ThisWorkspace sticky should remain on its workspace when active workspace changes: {client}"

        client_address = client["address"]
        assert driver.request("workspace-mode", rel=rel, mode="all").get("ok") is True
        client = wait_for(lambda: (c if c and c.get("pinned") else None)
                          if (c := next((x for x in clients(rig) if x["address"] == client_address), None)) else None,
                          "AllWorkspaces pin")
        lua_dispatch(rig, "hl.dsp.focus({workspace='3'})")
        wait_for(lambda: rig_json(rig, "activeworkspace").get("id") == 3, "workspace 3 activation")
        client = wait_client_address(rig, client_address)
        assert client.get("pinned") is True and client.get("visible") is True, \
            f"AllWorkspaces sticky should remain visible and pinned across workspace switch: {client}"

        assert driver.request("workspace-mode", rel=rel, mode="this").get("ok") is True
        lua_dispatch(rig, f"hl.dsp.window.move({{window='address:{client_address}',workspace='special:task6-live'}})")
        client = wait_for(lambda: (c if c.get("workspace", {}).get("name") == "special:task6-live" else None)
                          if (c := next((x for x in clients(rig) if x["address"] == client_address), None)) else None,
                          "named special workspace move")
        assert client.get("pinned") is False, f"special-workspace sticky unexpectedly pinned: {client}"
        toggle_result = lua_dispatch(rig, "hl.dsp.focus({workspace='special:task6-live'})")
        special_monitor_id = client["monitor"]
        def special_visible():
            current = next((m for m in rig_json(rig, "monitors") if m["id"] == special_monitor_id), None)
            return current if current and current.get("specialWorkspace", {}).get("name") == "special:task6-live" else None
        try:
            wait_for(special_visible, "special workspace visibility")
        except AssertionError as error:
            monitors = rig_json(rig, "monitors")
            state = {"dispatch": toggle_result,
                     "special_monitor": next((m for m in monitors if m["id"] == special_monitor_id), None),
                     "monitors": monitors, "workspaces": rig_json(rig, "workspaces"),
                     "active": rig_json(rig, "activeworkspace"), "clients": clients(rig)}
            raise AssertionError(f"special workspace did not toggle visible: {state}") from error

        assert driver.request("peer").get("ok") is True
        peer = wait_for(lambda: next((c for c in clients(rig) if c.get("initialTitle") == "task6-peer"), None), "fullscreen peer")
        peer_address = peer["address"]
        lua_dispatch(rig, f"hl.dsp.window.fullscreen({{window='address:{peer_address}',action='toggle'}})")
        peer = wait_for(lambda: (c if c.get("fullscreen", 0) != 0 else None)
                        if (c := next((x for x in clients(rig) if x["address"] == peer_address), None)) else None,
                        "fullscreen state on peer")
        client = wait_client_address(rig, client_address)
        assert client.get("fullscreen", 0) == 0, f"fullscreen toggle affected sticky instead of peer: {client}"
        lua_dispatch(rig, f"hl.dsp.window.fullscreen({{window='address:{peer_address}',action='toggle'}})")
        wait_for(lambda: next((c for c in clients(rig) if c["address"] == peer_address), {}).get("fullscreen", 0) == 0,
                 "peer fullscreen exit")
        assert driver.request("close-peer").get("ok") is True
        assert driver.request("close", rel=rel).get("ok") is True
        print("PASS workspaces: ThisWorkspace stays put, AllWorkspaces pinned visibility, special workspace, peer-only fullscreen")
    finally:
        app.stop()


def check_multi_monitor(rig, driver_path, scratch):
    app = LiveApp(driver_path, scratch)
    driver = app.start()
    created_rel = None
    secondary = None
    try:
        first = driver.request("new")
        first_rel = first["rel"]
        wait_role(rig, "sticky")
        before = rig_json(rig, "monitors")
        assert len(before) == 1, f"fresh rig should start with one output: {before}"
        primary = before[0]
        subprocess.check_call([rig, "hyprctl", "output", "create", "headless", "task6-secondary"], stdout=subprocess.DEVNULL)
        after = wait_for(lambda: (items if len(items) >= 2 else None) if (items := rig_json(rig, "monitors")) else None,
                         "second headless output")
        secondary = next((m for m in after if m["name"] != primary["name"]), None)
        assert secondary, f"new output missing from compositor: {after}"
        lua_monitor = f"hl.monitor({{output='{secondary['name']}',mode='preferred',position='1280x0',scale=1}})"
        subprocess.check_call([rig, "hyprctl", "eval", lua_monitor], stdout=subprocess.DEVNULL)
        subprocess.check_call([rig, "hyprctl", "reload"], stdout=subprocess.DEVNULL)
        secondary = output_monitor(rig, secondary["name"])

        assert driver.request("peer").get("ok") is True
        peer = wait_for(lambda: next((c for c in clients(rig) if c.get("initialTitle") == "task6-peer"), None), "monitor peer")
        lua_dispatch(rig, f"hl.dsp.window.move({{window='address:{peer['address']}',monitor='{secondary['name']}',follow=true}})")
        wait_for(lambda: active_window(rig).get("monitor") == secondary["id"], "active peer on secondary output")

        created = driver.request("new")
        created_rel = created["rel"]
        wait_role(rig, "sticky", 2)
        sticky = client_for_note(rig, driver, created_rel)
        sticky_address = sticky["address"]
        assert sticky.get("monitor") == secondary["id"], \
            f"new sticky should open on active output {secondary['name']}, got {sticky}"
        lua_dispatch(rig, f"hl.dsp.window.move({{window='address:{sticky['address']}',monitor='{primary['name']}',follow=false}})")
        sticky = wait_for(lambda: (c if c and c.get("monitor") == primary["id"] else None)
                          if (c := next((x for x in clients(rig) if x["address"] == sticky_address), None)) else None,
                          "cross-output move to primary")
        assert_client_inside_monitor(sticky, output_monitor(rig, primary["name"]))
        lua_dispatch(rig, f"hl.dsp.window.move({{window='address:{sticky_address}',monitor='{secondary['name']}',follow=false}})")
        sticky = wait_for(lambda: (c if c and c.get("monitor") == secondary["id"] else None)
                          if (c := next((x for x in clients(rig) if x["address"] == sticky_address), None)) else None,
                          "cross-output move to secondary")

        subprocess.check_call([rig, "hyprctl", "output", "remove", secondary["name"]], stdout=subprocess.DEVNULL)
        remaining = wait_for(lambda: (items if len(items) == 1 else None) if (items := rig_json(rig, "monitors")) else None,
                             "secondary output removal")
        sticky = wait_client_address(rig, sticky_address)
        assert sticky.get("monitor") == remaining[0]["id"], f"sticky did not survive output removal on remaining output: {sticky}"
        assert_client_inside_monitor(sticky, remaining[0])
        assert driver.request("close-peer").get("ok") is True
        print("PASS multi_monitor: active-output placement, cross-output moves, output removal survival and bounds")
    finally:
        app.stop()


def check_binds(rig, driver_path, scratch, app_binary, source_root):
    snippet = Path(source_root, "packaging/share/hyprland/hyprnotes.lua").read_text()
    expected = {
        "hyprnotes --new-note": ("SUPER + N", 64, "Hyprnotes: new note"),
        "hyprnotes --show-organizer": ("SUPER + SHIFT + N", 65, "Hyprnotes: organizer"),
    }
    lua_actions = {}
    for match in re.finditer(r'hl\.bind\("(SUPER \+ (?:SHIFT \+ )?N)"\s*,\s*hl\.dsp\.exec_cmd\("([^"]+)"\)', snippet):
        lua_actions[match.group(2)] = (match.group(1), match.group(2))
    assert set(lua_actions) == set(expected), f"unexpected packaged Lua actions: {lua_actions}"
    bind_table = rig_json(rig, "binds")
    for action, (keys, modmask, description) in expected.items():
        assert lua_actions[action][0] == keys, f"wrong key-to-action mapping in packaged Lua: {lua_actions}"
        key = "N"
        rows = [row for row in bind_table if row.get("dispatcher") == "__lua" and row.get("key") == key
                and row.get("modmask") == modmask and row.get("description") == description]
        assert len(rows) == 1 and rows[0].get("arg"), \
            f"expected one loaded Lua bind {keys} -> {action}; matching rows={rows}; table={bind_table}"

    env = scratch_env(Path(scratch, "app"))
    cfg = Path(env["XDG_CONFIG_HOME"], "hyprnotes")
    cfg.mkdir(parents=True, exist_ok=True)
    (cfg / "config.json").write_text('{"version":1,"trayEnabled":false}')
    bindir = Path(scratch, "bin")
    bindir.mkdir(parents=True, exist_ok=True)
    wrapper = bindir / "hyprnotes"
    lines = ["#!/bin/sh"]
    for key in ("HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME",
                "HN_CONFIG_DIR", "HN_DATA_DIR", "HN_STATE_DIR", "HN_CACHE_DIR", "HN_NOTES_DIR"):
        lines.append(f"export {key}={shlex.quote(env[key])}")
    lines.append(f"exec {shlex.quote(app_binary)} \"$@\"")
    wrapper.write_text("\n".join(lines) + "\n")
    wrapper.chmod(0o755)

    spawned_pid = None
    for action in expected:
        argv = shlex.split(action)
        assert argv[0] == "hyprnotes"
        command = " ".join([shlex.quote(str(wrapper)), *(shlex.quote(arg) for arg in argv[1:])])
        subprocess.check_call([rig, "hyprctl", "eval", f"hl.exec_cmd({json.dumps(command)})"], stdout=subprocess.DEVNULL)
        if argv[1] == "--new-note":
            sticky = wait_role(rig, "sticky")[0]
            spawned_pid = sticky["pid"]
            with open(Path(scratch, "pids"), "a", encoding="ascii") as pids:
                pids.write(f"{spawned_pid}\n")
        else:
            wait_role(rig, "organizer")
            org = by_role(rig, "organizer")[0]
            assert org["pid"] == spawned_pid, f"SUPER+SHIFT+N created a second app instead of dispatching to existing instance: {org}"
    sticky = by_role(rig, "sticky")[0]
    org = by_role(rig, "organizer")[0]
    assert sticky["pid"] == org["pid"] and sticky.get("floating") is True and org.get("floating") is True
    print("PASS binds: packaged Lua actions present in loaded bind table; exec dispatches created sticky and focused organizer in same app")


def check_reload_monitor_change(rig, driver_path, scratch):
    app = LiveApp(driver_path, scratch)
    driver = app.start()
    try:
        rel = driver.request("new")["rel"]
        assert driver.request("organizer").get("ok") is True
        wait_role(rig, "sticky")
        wait_role(rig, "organizer")
        monitor = rig_json(rig, "monitors")[0]
        monitor_name = monitor["name"]
        sticky = client_for_note(rig, driver, rel)
        organizer = by_role(rig, "organizer")[0]
        expected_sizes = {sticky["address"]: tuple(sticky["size"]), organizer["address"]: tuple(organizer["size"])}

        subprocess.check_call([rig, "hyprctl", "reload"], stdout=subprocess.DEVNULL)
        wait_for(lambda: len(by_role(rig, "sticky")) == 1 and len(by_role(rig, "organizer")) == 1, "clients after reload")
        before_change = output_monitor(rig, monitor_name)
        set_nested_monitor_config(scratch, "1600x900@60", "1.25")
        subprocess.check_call([rig, "hyprctl", "reload"], stdout=subprocess.DEVNULL)
        changed = wait_for(lambda: (m if abs(m.get("scale", 0) - 1.25) < 0.01 else None)
                           if (m := output_monitor(rig, monitor_name)) else None, "scale 1.25 activation")
        mode_change_supported = abs(changed["width"] - 1600) <= 3 and abs(changed["height"] - 900) <= 3

        for address, size in expected_sizes.items():
            client = wait_client_address(rig, address)
            assert tuple(client["size"]) == size, f"window size changed across monitor mode/scale update: {client}, before={size}"
            assert_client_inside_monitor(client, changed)

        subprocess.check_call([rig, "hyprctl", "reload"], stdout=subprocess.DEVNULL)
        restored = wait_for(lambda: (m if m.get("width", 0) > 0 and m.get("height", 0) > 0 else None)
                            if (m := output_monitor(rig, monitor_name)) else None, "monitor after second reload")
        assert restored["name"] == monitor_name and restored["id"] == before_change["id"]
        for address, size in expected_sizes.items():
            client = wait_client_address(rig, address)
            assert tuple(client["size"]) == size, f"window size changed after reload: {client}, before={size}"
            assert_client_inside_monitor(client, restored)
        assert driver.proc.poll() is None, "Qt app exited during reload or monitor change"
        if not mode_change_supported:
            print(f"INCONCLUSIVE reload_and_monitor_change: nested output remained {changed['width']}x{changed['height']} despite requested 1600x900; scale/reload bounds passed")
            return 2
        print("PASS reload_and_monitor_change: reload, 1280x960 mode/1.25 scale, retained sizes and bounded positions")
    finally:
        app.stop()


def image_mae_percent(left, right, scratch):
    size = subprocess.check_output(["magick", "identify", "-format", "%wx%h", left], text=True).strip()
    normalized = Path(scratch, "normalized-diff.png")
    subprocess.check_call(["magick", right, "-resize", size + "!", "-background", "white", "-alpha", "remove", str(normalized)])
    result = subprocess.run(["compare", "-metric", "MAE", left, str(normalized), "null:"],
                            text=True, capture_output=True, check=False)
    output = result.stderr + result.stdout
    match = re.search(r"\(([0-9.]+)%\)", output)
    if match:
        return float(match.group(1))
    match = re.search(r"\(([0-9.]+)\)", output)
    if not match:
        raise AssertionError(f"ImageMagick compare produced no MAE metric: {output}")
    return float(match.group(1)) * 100


def check_scales(rig, driver_path, scratch):
    app = LiveApp(driver_path, scratch)
    driver = None
    reference_dir = Path(scratch, "scale-reference")
    reference_dir.mkdir(parents=True, exist_ok=True)
    try:
        initial_monitors = rig_json(rig, "monitors")
        subprocess.check_call([rig, "hyprctl", "output", "create", "headless", "task6-scale"], stdout=subprocess.DEVNULL)
        monitors = wait_for(lambda: (items if len(items) > len(initial_monitors) else None)
                            if (items := rig_json(rig, "monitors")) else None, "dedicated scale headless output")
        monitor = next(m for m in monitors if m["name"] not in {old["name"] for old in initial_monitors})
        monitor_name = monitor["name"]
        scale_mode = "1280x720@60"
        set_nested_monitor_config(scratch, scale_mode, "1.0", output=monitor_name)
        subprocess.check_call([rig, "hyprctl", "reload"], stdout=subprocess.DEVNULL)
        monitor = wait_for(lambda: (m if m.get("width") == 1280 and m.get("height") == 720 else None)
                           if (m := output_monitor(rig, monitor_name)) else None, "scale monitor base mode")
        driver = app.start()
        assert driver.request("peer").get("ok") is True
        peer = wait_for(lambda: next((c for c in clients(rig) if c.get("initialTitle") == "task6-peer"), None),
                        "headless output focus peer")
        lua_dispatch(rig, f"hl.dsp.window.move({{window='address:{peer['address']}',monitor='{monitor_name}',follow=true}})")
        wait_for(lambda: active_window(rig).get("monitor") == monitor["id"], "focus peer on headless scale output")
        rel = driver.request("new")["rel"]
        assert driver.request("organizer").get("ok") is True
        assert driver.request("settings").get("ok") is True
        assert driver.request("consent").get("ok") is True
        wait_role(rig, "sticky")
        wait_role(rig, "organizer")
        app_windows = [c for c in clients(rig) if c.get("pid") == driver.proc.pid and c.get("mapped")
                       and c.get("initialTitle") != "task6-peer"]
        assert len(app_windows) >= 4, f"expected sticky, organizer, Settings and consent surfaces mapped: {app_windows}"
        assert all(c.get("monitor") == monitor["id"] for c in app_windows), \
            f"surfaces did not map naturally on focused test output: {app_windows}"
        wait_for(lambda: len([c for c in clients(rig) if c.get("pid") == driver.proc.pid and c.get("mapped")
                             and c.get("initialTitle") != "task6-peer"
                             and c.get("monitor") == monitor["id"]]) == len(app_windows),
                 "all test surfaces on dedicated headless output")
        assert driver.request("close-peer").get("ok") is True
        baseline = {}

        def capture(surface, key, scale):
            geometry = driver.request("geometry", surface=surface, rel=rel)
            assert geometry.get("ok") is True and geometry["width"] > 0 and geometry["height"] > 0, geometry
            assert geometry.get("screen") == monitor_name, f"{key} surface did not follow scale output: {geometry}"
            if geometry["screenWidth"] and geometry["screenHeight"]:
                assert geometry["x"] >= geometry["screenX"] - 2 and geometry["y"] >= geometry["screenY"] - 2 and \
                    geometry["x"] + geometry["width"] <= geometry["screenX"] + geometry["screenWidth"] + 2 and \
                    geometry["y"] + geometry["height"] <= geometry["screenY"] + geometry["screenHeight"] + 2, \
                    f"{key} surface outside available screen at scale {scale}: {geometry}"
            if surface == "consent":
                assert geometry["height"] <= int(geometry["screenHeight"] * 0.9) + 2, \
                    f"consent exceeds 90% of available screen height at scale {scale}: {geometry}"
            for child in geometry["children"]:
                if child["scrollArea"]:
                    continue
                assert child["x"] >= -2 and child["y"] >= -2 and \
                    child["x"] + child["width"] <= child["parentWidth"] + 2 and \
                    child["y"] + child["height"] <= child["parentHeight"] + 2, \
                    f"clipped {key} child at scale {scale}: {child}"
            path = Path(scratch, f"{key}-{scale}.png")
            shot = driver.request("screenshot", surface=surface, rel=rel, path=str(path))
            assert shot.get("ok") and path.is_file() and path.stat().st_size > 100, shot
            if scale == 1.0:
                reference = reference_dir / f"{key}.png"
                reference.write_bytes(path.read_bytes())
                baseline[key] = reference
            else:
                diff = image_mae_percent(str(baseline[key]), str(path), scratch)
                assert diff <= 10.0, f"{key} image diff at scale {scale} is {diff:.2f}% (fixed tolerance 10%)"

        for scale in (1.0, 1.25, 1.5, 2.0):
            scaled_mode = f"{round(1280 * scale)}x{round(720 * scale)}@60"
            set_nested_monitor_config(scratch, scaled_mode, scale, output=monitor_name)
            subprocess.check_call([rig, "hyprctl", "reload"], stdout=subprocess.DEVNULL)
            try:
                current_monitor = wait_for(lambda: (m if abs(m.get("scale", 0) - scale) < 0.01 else None)
                                           if (m := output_monitor(rig, monitor_name)) else None,
                                           f"monitor scale {scale}", timeout=5)
            except AssertionError as error:
                current = output_monitor(rig, monitor_name)
                log = Path(scratch, "hypr.log").read_text(errors="replace")[-4000:]
                raise AssertionError(f"nested config requested scale {scale}, but monitor stayed {current}; log={log}") from error
            assert (current_monitor["width"], current_monitor["height"]) == (round(1280*scale), round(720*scale)), \
                f"test output mode mismatch at scale {scale}: {current_monitor}"
            for surface in ("organizer", "sticky", "consent"):
                capture(surface, surface, scale)
            for tab in range(5):
                assert driver.request("settings-tab", index=tab).get("ok") is True
                capture("settings", f"settings-tab-{tab}", scale)

            for role in ("sticky", "organizer"):
                found = by_role(rig, role)
                assert len(found) == 1, f"scale {scale}: missing {role} client: {found}"
                assert_client_inside_monitor(found[0], current_monitor)
            print(f"scale {scale}: organizer/sticky/5 Settings tabs/consent geometry passed; normalized image MAE <= 10%")
        assert driver.proc.poll() is None
        print("PASS scales: organizer, sticky, Settings, consent at 1/1.25/1.5/2; screenshots and image-diff tolerance")
    finally:
        app.stop()


class _UnixServer(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
    daemon_threads = True
    allow_reuse_address = True


class _FakeRequestHandler(socketserver.BaseRequestHandler):
    def handle(self):
        owner = self.server.owner
        command = self.request.recv(65536).decode(errors="replace")
        owner.request_count += 1
        try:
            if command.startswith("j/clients"):
                body = owner.clients_provider()
            elif command.startswith("j/monitors"):
                body = owner.monitors_provider()
            elif command.startswith("j/workspaces"):
                body = owner.workspaces_provider()
            elif command.startswith("j/activewindow"):
                body = {}
            else:
                body = "ok"
            if isinstance(body, (dict, list)):
                body = json.dumps(body, separators=(",", ":"))
            self.request.sendall(str(body).encode())
        except (OSError, ValueError, subprocess.SubprocessError):
            try:
                self.request.sendall(b"[]")
            except OSError:
                pass


class _FakeEventHandler(socketserver.BaseRequestHandler):
    def handle(self):
        owner = self.server.owner
        with owner.lock:
            owner.event_connection = self.request
            owner.event_connected.set()
        try:
            while self.request.recv(65536):
                pass
        except OSError:
            pass
        finally:
            with owner.lock:
                if owner.event_connection is self.request:
                    owner.event_connection = None
                    owner.event_connected.clear()


class FakeHyprIpc:
    def __init__(self, directory, clients_provider, monitors_provider, workspaces_provider):
        self.directory = Path(directory)
        self.clients_provider = clients_provider
        self.monitors_provider = monitors_provider
        self.workspaces_provider = workspaces_provider
        self.lock = threading.Lock()
        self.request_count = 0
        self.event_connection = None
        self.event_connected = threading.Event()
        self.request_server = None
        self.event_server = None
        self.threads = []

    def start(self):
        self.directory.mkdir(parents=True, exist_ok=True)
        for name in (".socket.sock", ".socket2.sock"):
            path = self.directory / name
            if path.exists():
                path.unlink()
        self.request_server = _UnixServer(str(self.directory / ".socket.sock"), _FakeRequestHandler)
        self.event_server = _UnixServer(str(self.directory / ".socket2.sock"), _FakeEventHandler)
        self.request_server.owner = self
        self.event_server.owner = self
        for server in (self.request_server, self.event_server):
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            self.threads.append(thread)
        return self

    def send_events(self, payload):
        with self.lock:
            connection = self.event_connection
        if not connection:
            raise RuntimeError("app event socket is not connected")
        connection.sendall(payload)

    def stop(self):
        with self.lock:
            connection = self.event_connection
            self.event_connection = None
            self.event_connected.clear()
        if connection:
            try:
                connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            connection.close()
        for server in (self.event_server, self.request_server):
            if server:
                server.shutdown()
                server.server_close()
        self.event_server = self.request_server = None
        for name in (".socket.sock", ".socket2.sock"):
            try:
                (self.directory / name).unlink()
            except FileNotFoundError:
                pass


def host_workspace_id():
    env = dict(os.environ)
    env["HYPRLAND_INSTANCE_SIGNATURE"] = env["HN_NESTED_HOST_SIG"]
    return json.loads(subprocess.check_output(["hyprctl", "-j", "activeworkspace"], env=env, text=True))["id"]


def pss_bytes(pid):
    try:
        for line in Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines():
            if line.startswith("Pss:"):
                return int(line.split()[1]) * 1024
    except (OSError, ValueError, IndexError):
        return None
    return None


def process_cpu_ticks(pid):
    raw = Path(f"/proc/{pid}/stat").read_text()
    rest = raw.rpartition(")")[2].split()
    return int(rest[11]) + int(rest[12])


def fake_server_for_rig(rig, directory):
    return FakeHyprIpc(directory,
                       lambda: rig_json(rig, "clients"),
                       lambda: rig_json(rig, "monitors"),
                       lambda: rig_json(rig, "workspaces"))


def selftest_fake_ipc():
    with tempfile.TemporaryDirectory(prefix="hyprnotes-fake-ipc-") as temp:
        server = FakeHyprIpc(Path(temp, "ipc"), lambda: [], lambda: [], lambda: []).start()
        try:
            request = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            request.connect(str(Path(temp, "ipc", ".socket.sock")))
            request.sendall(b"j/clients")
            assert request.recv(4096) == b"[]"
            request.close()

            event = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            event.connect(str(Path(temp, "ipc", ".socket2.sock")))
            wait_for(lambda: server.event_connected.is_set(), "fake event client", timeout=2)
            payload = b"openwindow>>bad\xff\nopenwindow>>valid\n"
            server.send_events(payload)
            assert event.recv(len(payload)) == payload
            event.close()
        finally:
            server.stop()
    print("PASS fake IPC server offline selftest: request JSON and binary/newline event stream")


def check_ipc_garbage(rig, driver_path, scratch):
    fake = fake_server_for_rig(rig, Path(scratch, "fake-ipc")).start()
    app = LiveApp(driver_path, scratch)
    driver = None
    try:
        driver = app.start({"HN_LIVE_HYPR_IPC_DIR": str(fake.directory)})
        rel = driver.request("new")["rel"]
        wait_role(rig, "sticky")
        assert fake.event_connected.wait(5), "HyprlandIpc never opened fake event socket"
        warm = driver.request("edit", rel=rel, text="garbage-baseline")
        assert warm.get("ok") and warm.get("text", "").endswith("garbage-baseline"), warm
        assert driver.request("save-note", rel=rel).get("ok") is True
        note_path = Path(app.env["HN_NOTES_DIR"], rel)
        wait_for(lambda: note_path.exists() and b"garbage-baseline" in note_path.read_bytes(), "warm-up note save")
        time.sleep(1)
        baseline = pss_bytes(driver.proc.pid)
        assert baseline is not None, "cannot read driver PSS baseline"
        before_queries = fake.request_count

        oversized = b"openwindow>>" + b"A" * ((1 << 20) + 8192) + b"\n"
        malformed = b"missing-separator\n\xff\xfe\nopenwindow>>\xff\n"
        burst = b"".join(f"openwindow>>burst-{i}\n".encode() for i in range(10000))
        fake.send_events(malformed + oversized + burst + b"openwindow>>valid-after-garbage\n")
        wait_for(lambda: fake.request_count > before_queries, "valid event follow-up query")
        time.sleep(0.5)
        assert driver.proc.poll() is None, f"application exited on malformed event stream: {driver.proc.poll()}"
        state = driver.request("state")
        assert rel in state.get("sessions", []) and len(by_role(rig, "sticky")) == 1, \
            f"sticky/session lost after malformed event stream: {state}"
        edited = driver.request("edit", rel=rel, text="garbage-survived")
        assert edited.get("ok") and edited.get("text", "").endswith("garbage-survived"), edited
        assert driver.request("save-note", rel=rel).get("ok") is True
        expected = edited["text"].encode() + b"\n"
        wait_for(lambda: Path(app.env["HN_NOTES_DIR"], rel).read_bytes() == expected, "edited file after garbage events")
        time.sleep(0.5)
        after = pss_bytes(driver.proc.pid)
        assert after is not None and after - baseline < 5 * 1024 * 1024, \
            f"PSS grew {(after - baseline) / (1024 * 1024):.2f} MiB after malformed/oversized/burst events"
        print(f"PASS ipc_garbage: malformed/binary/>1MiB+valid-followup/10k event burst; PSS delta={(after-baseline)/1048576:.2f} MiB")
    finally:
        if app.driver:
            app.stop()
        fake.stop()


def check_event_storm(rig, driver_path, scratch):
    app = LiveApp(driver_path, scratch)
    driver = app.start()
    event_sock = None
    try:
        rel = driver.request("new")["rel"]
        wait_role(rig, "sticky")
        sticky = client_for_note(rig, driver, rel)
        base = Path(os.environ["XDG_RUNTIME_DIR"], "hypr", os.environ["HYPRLAND_INSTANCE_SIGNATURE"], ".socket2.sock")
        event_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        event_sock.settimeout(0.2)
        event_sock.connect(str(base))
        event_sock.setblocking(False)
        pss_before = pss_bytes(driver.proc.pid)
        assert pss_before is not None
        event_opens, event_closes, seen_titles = 0, 0, set()
        latencies = []
        start = time.monotonic()
        buffer = bytearray()
        progress = driver.request("storm-start", count=1000)
        assert progress.get("ok") is True, progress
        progress = driver.request("storm-progress")

        def drain_events():
            nonlocal event_opens, event_closes
            try:
                while True:
                    data = event_sock.recv(65536)
                    if not data:
                        break
                    buffer.extend(data)
            except BlockingIOError:
                pass
            while b"\n" in buffer:
                line, _, tail = buffer.partition(b"\n")
                buffer[:] = tail
                if b">>" not in line:
                    continue
                name, payload = line.split(b">>", 1)
                fields = payload.decode(errors="replace").split(",")
                if name == b"openwindow" and len(fields) >= 4 and fields[3].startswith("task6-storm-"):
                    event_opens += 1
                elif name == b"closewindow" and fields:
                    event_closes += 1

        while progress.get("active"):
            if progress.get("waitingForRelease"):
                expected_batch = progress.get("batchSize", 0)
                mapped = wait_for(lambda: (rows if len(rows) == expected_batch else None)
                                  if (rows := [c for c in clients(rig) if c.get("initialTitle", "").startswith("task6-storm-")]) else None,
                                  f"{expected_batch} nested Hyprland storm clients", timeout=8)
                assert len(mapped) == expected_batch, mapped
                seen_titles.update(c["initialTitle"] for c in mapped)
                latency = driver.request("latency", rel=rel)
                assert latency.get("ok"), latency
                latencies.append(latency["milliseconds"])
                assert driver.request("storm-release").get("ok") is True
                wait_for(lambda: not any(c.get("initialTitle", "").startswith("task6-storm-") for c in clients(rig)),
                         "storm batch unmap", timeout=8)
                drain_events()
                progress = driver.request("storm-progress")
                if progress.get("active") and progress.get("created") < 1000:
                    assert driver.request("storm-next").get("ok") is True
                    progress = driver.request("storm-progress")
            else:
                drain_events()
                progress = driver.request("storm-progress")
            if time.monotonic() - start > 90:
                raise TimeoutError(f"window storm exceeded 90 seconds: {progress}; mapped titles={len(seen_titles)}")
        drain_events()
        progress = driver.request("storm-progress")
        assert progress.get("created") == 1000 and progress.get("closed") == 1000, f"window storm counters: {progress}"
        assert len(seen_titles) == 1000, f"nested hyprctl JSON observed only {len(seen_titles)} unique mapped storm windows"
        assert event_opens > 0 and event_closes > 0, f"no map/unmap events observed: open={event_opens} close={event_closes}"
        assert latencies and max(latencies) < 100, f"typing latency exceeded 100 ms: {latencies}"
        wait_for(lambda: not any(c.get("initialTitle", "").startswith("task6-storm-") for c in clients(rig)),
                 "all storm windows closed")
        time.sleep(1)
        pss_after = pss_bytes(driver.proc.pid)
        assert pss_after is not None and pss_after - pss_before < 5 * 1024 * 1024, \
            f"post-storm PSS delta {(pss_after-pss_before)/1048576:.2f} MiB"
        survive = client_for_note(rig, driver, rel)
        assert survive["address"] == sticky["address"] and driver.proc.poll() is None, \
            f"sticky/app lost in window storm: {survive}"
        print(f"PASS event_storm: 1000 compositor-observed windows, event tap {event_opens} open/{event_closes} close, max typing {max(latencies)} ms, PSS delta {(pss_after-pss_before)/1048576:.2f} MiB")
    finally:
        if event_sock:
            event_sock.close()
        app.stop()


def check_ipc_loss(rig, driver_path, scratch):
    initial_host_ws = host_workspace_id()
    fake_dir = Path(scratch, "fake-ipc")
    fake = fake_server_for_rig(rig, fake_dir).start()
    app = LiveApp(driver_path, scratch)
    try:
        driver = app.start({"HN_LIVE_HYPR_IPC_DIR": str(fake_dir)})
        rels = [driver.request("new")["rel"] for _ in range(3)]
        wait_role(rig, "sticky", 3)
        assert fake.event_connected.wait(5), "app did not connect fake Hyprland event socket"
        main_rel = rels[0]
        typed = driver.request("edit", rel=main_rel, text="before-ipc-loss")
        assert typed.get("ok") and typed.get("text", "").endswith("before-ipc-loss"), typed
        assert driver.request("save-note", rel=main_rel).get("ok") is True
        note_path = Path(app.env["HN_NOTES_DIR"], main_rel)
        saved = note_path.read_bytes()
        assert b"before-ipc-loss" in saved

        before_queries = fake.request_count
        fake.stop()
        ticks0 = process_cpu_ticks(driver.proc.pid)
        cpu_start = time.monotonic()
        time.sleep(3)
        elapsed = time.monotonic() - cpu_start
        display_loss = "frontend remained connected"
        if driver.proc.poll() is None:
            ticks1 = process_cpu_ticks(driver.proc.pid)
            cpu_percent = (ticks1 - ticks0) / os.sysconf("SC_CLK_TCK") / elapsed * 100
            assert cpu_percent < 5.0, f"Hyprland IPC disconnect spun CPU at {cpu_percent:.2f}%"
            latency = driver.request("latency", rel=main_rel)
            assert latency.get("ok") and latency["milliseconds"] < 100, f"typing probe during IPC outage: {latency}"
            typed = driver.request("edit", rel=main_rel, text="-while-disconnected")
            assert typed.get("ok") and typed.get("text", "").endswith("-while-disconnected"), typed
            assert driver.request("save-note", rel=main_rel).get("ok") is True
        else:
            cpu_percent = 0.0
            assert driver.proc.returncode == 0, \
                f"frontend crashed on fake Hyprland IPC loss while Wayland remained alive: exit={driver.proc.returncode}"
            assert b"before-ipc-loss" in note_path.read_bytes(), "orderly exit lost previously saved note"

        fake.start()
        assert fake.event_connected.wait(10), "HyprlandIpc did not reconnect after fake event server returned"
        wait_for(lambda: fake.request_count >= before_queries + 3, "re-enumeration queries after IPC reconnect", timeout=12)
        fake.send_events(b"openwindow>>reconnected\n")
        wait_for(lambda: fake.request_count >= before_queries + 6, "state queries after valid reconnect event", timeout=12)
        assert driver.proc.poll() is None, "app exited after fake IPC reconnection"
        assert len(driver.request("state").get("sessions", [])) == 3
        assert len([c for c in clients(rig) if c.get("class") == "hyprnotes"]) == 3
        fake.stop()

        assert driver.request("flush").get("ok") is True
        Path(scratch, "expected-host-window-loss").touch()
        old_hypr_pid = int(Path(scratch, "hypr.pid").read_text())
        # Exact-identity pidfd signal through the controller's recorded compositor row.
        session = Path(os.environ["HN_LIVE_SESSION_DIR"])
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import process_guard
        state = json.loads((session / "state.json").read_text())
        row = next(json.loads((session / r["name"]).read_text()) for r in state["records"]
                   if r["kind"] == "compositor" and r["pid"] == old_hypr_pid)
        assert process_guard.signal_record(row, str(session / "hyprland.lua")), "guard refused compositor stop"
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and driver.proc.poll() is None:
            time.sleep(0.05)
        if driver.proc.poll() is None:
            ticks0 = process_cpu_ticks(driver.proc.pid)
            cpu_start = time.monotonic()
            time.sleep(3)
            ticks1 = process_cpu_ticks(driver.proc.pid)
            cpu_percent = (ticks1 - ticks0) / os.sysconf("SC_CLK_TCK") / (time.monotonic() - cpu_start) * 100
            assert cpu_percent < 5.0, f"Qt frontend spun CPU after nested Wayland display loss: {cpu_percent:.2f}%"
            try:
                latency = driver.request("latency", rel=main_rel)
                print(f"nested display-loss typing probe: {latency}")
            except (TimeoutError, RuntimeError, BrokenPipeError) as error:
                print(f"nested display-loss typing unavailable: {error}")
        else:
            code = driver.proc.returncode
            diagnostic = driver.proc.stderr.read().decode(errors="replace")
            if code not in (0, 255) and not re.search(r"wayland|wl_display|display connection", diagnostic, re.IGNORECASE):
                raise AssertionError(f"Qt frontend exited {code} without a Wayland-loss diagnostic: {diagnostic[-1000:]}")
            display_loss = f"Qt frontend exited after Wayland display loss (status {code}; further typing unavailable)"
            print(display_loss + "; IPC-only typing/reconnect was separately verified above")
        app.stop()

        if host_workspace_id() != initial_host_ws:
            print(f"INCONCLUSIVE ipc_loss: host workspace drifted {initial_host_ws}; refusing to start replacement compositor")
            return 2
        # Same controller session: scratch (notes) must survive, so restart instead of stop+start.
        started = subprocess.run([rig, "restart"], text=True, capture_output=True)
        if started.returncode == 2:
            print("INCONCLUSIVE ipc_loss: nested rig refused restart after detecting host workspace drift")
            return 2
        assert started.returncode == 0, f"owned nested compositor failed to restart: {started.stdout} {started.stderr}"
        Path(scratch, "expected-host-window-loss").unlink(missing_ok=True)
        refreshed_env = {}
        for line in subprocess.check_output([rig, "env"], text=True).splitlines():
            if line.startswith("export "):
                key, value = shlex.split(line)[1].split("=", 1)
                refreshed_env[key] = value
        restored_app = LiveApp(driver_path, scratch)
        restored_driver = restored_app.start(refreshed_env)
        wait_role(rig, "sticky", 3)
        state = restored_driver.request("state")
        assert len(state.get("sessions", [])) == 3 and len(state.get("windows", [])) == 3, state
        for rel in rels:
            assert Path(restored_app.env["HN_NOTES_DIR"], rel).exists()
        print(f"PASS ipc_loss: IPC-only outage/reconnect typed and re-enumerated; {display_loss}; replacement restored 3 windows (CPU {cpu_percent:.2f}%)")
        restored_app.stop()
        return 0
    finally:
        fake.stop()
        app.stop()


def bus_property(destination, path, interface, property_name):
    return subprocess.check_output(["busctl", "--user", "get-property", destination, path, interface, property_name],
                                   text=True, stderr=subprocess.DEVNULL).strip()


def registered_sni():
    raw = bus_property("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
                       "org.kde.StatusNotifierWatcher", "RegisteredStatusNotifierItems")
    for item in re.findall(r'"([^"]+)"', raw):
        if "/" not in item:
            continue
        service, path = item.split("/", 1)
        path = "/" + path
        try:
            item_id = bus_property(service, path, "org.kde.StatusNotifierItem", "Id").lower()
        except subprocess.CalledProcessError:
            continue
        if "hyprnotes" in item_id:
            return service, path
    return None


def check_tray(rig, driver_path, scratch):
    assert os.environ.get("DBUS_SESSION_BUS_ADDRESS"), "tray check must run inside dbus-run-session"
    assert shutil.which("waybar") and shutil.which("busctl"), "Waybar and busctl are required"
    app = LiveApp(driver_path, scratch)
    waybar = None
    driver = None
    waybar_cfg = Path(scratch, "waybar.json")
    waybar_css = Path(scratch, "waybar.css")
    waybar_log = Path(scratch, "waybar.log")
    waybar_cfg.write_text('{"layer":"top","position":"top","modules-left":["tray"]}')
    waybar_css.write_text("* { min-height: 0; }\n")

    def start_waybar():
        process = subprocess.Popen(["waybar", "-c", str(waybar_cfg), "-s", str(waybar_css)],
                                   stdout=waybar_log.open("ab"), stderr=subprocess.STDOUT)
        with open(Path(scratch, "pids"), "a", encoding="ascii") as pids:
            pids.write(f"{process.pid}\n")
        return process

    def wait_item(timeout=10):
        return wait_for(registered_sni, "private-bus Hyprnotes StatusNotifier item", timeout)

    try:
        waybar = start_waybar()
        time.sleep(0.5)
        assert waybar.poll() is None, f"Waybar exited: {waybar_log.read_text(errors='replace')}"
        driver = app.start({"HN_LIVE_ENABLE_TRAY": "1"})
        created = driver.request("new")
        rel = created["rel"]
        wait_role(rig, "sticky")
        item = wait_item()
        service, object_path = item
        icon_before = bus_property(service, object_path, "org.kde.StatusNotifierItem", "IconPixmap")
        current = driver.request("theme", scheme="light")
        assert current.get("ok") is True, current
        current = driver.request("theme", scheme="dark")
        assert current.get("ok") is True, current
        icon_after = wait_for(lambda: (value if value != icon_before else None)
                              if (value := bus_property(service, object_path, "org.kde.StatusNotifierItem", "IconPixmap")) else None,
                              "theme-updated SNI icon pixmap")
        assert icon_after != icon_before, "tray icon pixmap did not change when application color scheme changed"

        subprocess.check_call(["gdbus", "call", "--session", "--dest", service, "--object-path", object_path,
                               "--method", "org.kde.StatusNotifierItem.ContextMenu", "0", "0"],
                              stdout=subprocess.DEVNULL)
        menu_path = re.search(r'"(/[^\"]*)"', bus_property(service, object_path,
                                                           "org.kde.StatusNotifierItem", "Menu"))
        assert menu_path, "StatusNotifierItem returned no DBusMenu path for right-click context menu"
        layout = subprocess.check_output(["busctl", "--user", "call", service, menu_path.group(1),
                                          "com.canonical.dbusmenu", "GetLayout", "iias", "0", "1", "0"], text=True)
        for label in ("New Note", "Show Organizer", "Quit"):
            assert label in layout, f"right-click menu missing {label}: {layout}"

        subprocess.check_call(["gdbus", "call", "--session", "--dest", service, "--object-path", object_path,
                               "--method", "org.kde.StatusNotifierItem.Activate", "0", "0"],
                              stdout=subprocess.DEVNULL)
        wait_role(rig, "organizer")
        assert driver.proc.poll() is None

        waybar.terminate()
        waybar.wait(timeout=5)
        waybar = start_waybar()
        assert waybar.poll() is None
        service_after, path_after = wait_item()
        assert (service_after, path_after) == item, f"Waybar restart changed/lost the SNI registration: {item} -> {(service_after, path_after)}"

        waybar.terminate()
        waybar.wait(timeout=5)
        extra = driver.request("new")
        wait_role(rig, "sticky", 2)
        assert extra.get("ok") and driver.proc.poll() is None
        print("PASS tray_waybar: private DBus registration, right-click menu, left activation, themed icon, Waybar restart, no-host app")
    finally:
        if waybar and waybar.poll() is None:
            waybar.terminate()
            waybar.wait(timeout=5)
        app.stop()


def wait_for(predicate, message, timeout=12):
    end = time.monotonic() + timeout
    last = None
    while time.monotonic() < end:
        try:
            last = predicate()
            if last:
                return last
        except (subprocess.CalledProcessError, json.JSONDecodeError):
            pass
        time.sleep(0.1)
    raise AssertionError(f"timed out waiting for {message}; last={last}")


class LiveApp:
    def __init__(self, driver_path, scratch):
        self.driver_path = driver_path
        self.scratch = scratch
        self.driver = None
        self.env = None

    def start(self, extra_env=None):
        env = scratch_env(Path(self.scratch, "app"))
        if extra_env:
            env.update(extra_env)
        self.env = env
        Path(self.scratch, "pids").parent.mkdir(parents=True, exist_ok=True)
        self.driver = Driver([self.driver_path], env)
        with open(Path(self.scratch, "pids"), "a", encoding="ascii") as pids:
            pids.write(f"{self.driver.proc.pid}\n")
        return self.driver

    def terminate(self, force=False):
        if not self.driver:
            return
        process = self.driver.proc
        if process.poll() is None:
            os.kill(process.pid, signal.SIGKILL if force else signal.SIGTERM)
            result = process.wait(timeout=12)
            expected = -signal.SIGKILL if force else 0
            assert result == expected, f"process exit status after {'SIGKILL' if force else 'SIGTERM'}: {result}, expected {expected}"
        self.driver.selector.close()
        self.driver = None

    def stop(self):
        if self.driver:
            self.driver.stop()
            self.driver = None


def assert_windows_equal_sessions(reply):
    assert reply.get("ok") is True, reply
    sessions = reply.get("sessions", [])
    windows = reply.get("windows", [])
    assert len(sessions) == len(set(sessions)), f"duplicate open sessions: {sessions}"
    notes = [window["rel"] for window in windows]
    assert len(notes) == len(set(notes)), f"a session has multiple visible windows: {windows}"
    assert sorted(notes) == sorted(sessions), f"session/window mismatch: sessions={sessions}, windows={windows}"


def check_rules(rig, driver_path, scratch):
    app = LiveApp(driver_path, scratch)
    driver = app.start()
    try:
        created = driver.request("new")
        assert created.get("ok") and created.get("rel"), created
        assert driver.request("organizer").get("ok") is True
        wait_for(lambda: len(by_role(rig, "sticky")) == 1 and len(by_role(rig, "organizer")) == 1,
                 "sticky and organizer windows")
        clients_by_title = clients(rig)
        for role, size in (("sticky", (360, 300)), ("organizer", (960, 640))):
            actual = next((item for item in clients_by_title if item.get("initialTitle", "").startswith(f"hyprnotes-{role}:")), None)
            assert actual is not None, f"missing {role}: {clients_by_title}"
            assert actual.get("floating") is True, f"{role} rule did not float: {actual}"
            assert tuple(actual.get("size", [])) == size, f"{role} rule size {actual.get('size')} != {size}: {actual}"
        organizer = by_role(rig, "organizer")[0]
        monitor = next(m for m in rig_json(rig, "monitors") if m["id"] == organizer["monitor"])
        x, y = organizer["at"]
        w, h = organizer["size"]
        assert abs(x + w / 2 - monitor["width"] / 2) <= 3, f"organizer not centered horizontally: {organizer}, {monitor}"
        assert abs(y + h / 2 - monitor["height"] / 2) <= 3, f"organizer not centered vertically: {organizer}, {monitor}"
        print("PASS rules_applied: generated sticky/organizer titles matched Lua rules; hyprctl state confirms float, size, center")
    finally:
        app.stop()


def check_lifecycle(rig, driver_path, scratch):
    app = LiveApp(driver_path, scratch)
    driver = app.start()
    rel = ""
    try:
        created = driver.request("new")
        assert created.get("ok") and created.get("rel"), created
        rel = created["rel"]
        note_path = Path(app.env["HN_NOTES_DIR"], rel)
        wait_for(lambda: note_path.exists(), "new note file")
        assert note_path.read_bytes() == b"", f"new note did not start empty: {note_path.read_bytes()!r}"
        wait_for(lambda: len(by_role(rig, "sticky")) == 1, "new sticky client")

        marker = "task6-lifecycle"
        edited = driver.request("edit", rel=rel, text=marker)
        assert edited.get("ok") and edited.get("text", "").endswith(marker), edited
        saved = driver.request("save-note", rel=rel)
        assert saved.get("ok") is True and saved.get("state") == "Saved", saved
        expected_bytes = marker.encode() + b"\n"
        wait_for(lambda: note_path.read_bytes() == expected_bytes, "exact saved note bytes")
        assert note_path.read_bytes() == expected_bytes, f"note bytes differ: {note_path.read_bytes()!r}"
        assert_windows_equal_sessions(driver.request("state"))

        assert driver.request("pop-in", rel=rel).get("ok") is True
        wait_for(lambda: len(by_role(rig, "sticky")) == 0 and len(by_role(rig, "organizer")) == 1, "pop-in organizer state")
        assert_windows_equal_sessions(driver.request("state"))
        assert driver.request("pop-out", rel=rel).get("ok") is True
        wait_for(lambda: len(by_role(rig, "sticky")) == 1, "pop-out sticky state")
        assert_windows_equal_sessions(driver.request("state"))

        assert driver.request("flush").get("ok") is True
        session_path = Path(app.env["HN_STATE_DIR"], "session.json")
        session = json.loads(session_path.read_text())
        assert [item["note"] for item in session["windows"]] == [rel], session
        app.terminate(force=False)
        assert app.driver is None

        driver = app.start()
        wait_for(lambda: len(by_role(rig, "sticky")) == 1, "SIGTERM session restore")
        assert note_path.read_bytes() == expected_bytes, "SIGTERM restore changed saved note bytes"
        assert_windows_equal_sessions(driver.request("state"))
        assert driver.request("flush").get("ok") is True
        app.terminate(force=True)

        driver = app.start()
        wait_for(lambda: len(by_role(rig, "sticky")) == 1, "SIGKILL session restore")
        assert note_path.read_bytes() == expected_bytes, "SIGKILL restore changed saved note bytes"
        assert_windows_equal_sessions(driver.request("state"))
        assert driver.request("close", rel=rel).get("ok") is True
        wait_for(lambda: not any(item.get("initialTitle", "").startswith("hyprnotes-sticky:") for item in clients(rig)),
                 "closed sticky client")
        state = driver.request("state")
        assert state.get("sessions") == [] and state.get("windows") == [], state
        assert note_path.read_bytes() == expected_bytes, "closing note changed exact saved content"
        print("PASS sticky_lifecycle: create/edit/exact save, pop-in/out, session-window parity, SIGTERM/SIGKILL restore, close")
    finally:
        app.stop()


if __name__ == "__main__":
    if sys.argv[1:] == ["--selftest-ipc"]:
        selftest_fake_ipc()
        raise SystemExit(0)
    if len(sys.argv) < 5 or len(sys.argv) > 7:
        raise SystemExit("usage: task6_driver_checks.py MODE RIG DRIVER SCRATCH [APP_BINARY [SOURCE_ROOT]]")
    mode, rig_path, driver_bin, scratch_dir = sys.argv[1:5]
    if mode == "rules" and len(sys.argv) == 5:
        check_rules(rig_path, driver_bin, scratch_dir)
    elif mode == "lifecycle" and len(sys.argv) == 5:
        check_lifecycle(rig_path, driver_bin, scratch_dir)
    elif mode == "focus" and len(sys.argv) == 5:
        check_focus_and_stacking(rig_path, driver_bin, scratch_dir)
    elif mode == "workspaces" and len(sys.argv) == 5:
        check_workspaces(rig_path, driver_bin, scratch_dir)
    elif mode == "multi_monitor" and len(sys.argv) == 5:
        check_multi_monitor(rig_path, driver_bin, scratch_dir)
    elif mode == "reload" and len(sys.argv) == 5:
        check_reload_monitor_change(rig_path, driver_bin, scratch_dir)
    elif mode == "scales" and len(sys.argv) == 5:
        check_scales(rig_path, driver_bin, scratch_dir)
    elif mode == "tray" and len(sys.argv) == 5:
        check_tray(rig_path, driver_bin, scratch_dir)
    elif mode == "ipc_garbage" and len(sys.argv) == 5:
        check_ipc_garbage(rig_path, driver_bin, scratch_dir)
    elif mode == "event_storm" and len(sys.argv) == 5:
        check_event_storm(rig_path, driver_bin, scratch_dir)
    elif mode == "ipc_loss" and len(sys.argv) == 5:
        raise SystemExit(check_ipc_loss(rig_path, driver_bin, scratch_dir))
    elif mode == "binds" and len(sys.argv) == 7:
        check_binds(rig_path, driver_bin, scratch_dir, sys.argv[5], sys.argv[6])
    else:
        raise SystemExit(f"invalid mode arguments: {mode}")
