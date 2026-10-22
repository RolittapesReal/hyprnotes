#!/usr/bin/env python3
"""Argument-preserving command encoders shared by harness and offline tests."""
import json
import os
import shlex
import sys


def launcher(directory, token, aq):
    args = aq.split()
    config = os.path.realpath(directory + "/hyprland.lua")
    words = [*args, "Hyprland", "-c", config]
    command = " ".join(shlex.quote(word) for word in words)
    return ("#!/bin/bash\n"
            f"export HN_LIVE_OWNER_TOKEN={shlex.quote(token)}\n"
            f"echo \"$BASHPID\" > {shlex.quote(directory + '/hypr.pid')}\n"
            "exec env -u HYPRLAND_INSTANCE_SIGNATURE " + command +
                f" >{shlex.quote(directory + '/hypr.log')} 2>&1\n")


def nested_launcher(scratch, session, executable, args, aq):
    """Build launcher whose argv is the exact post-exec intent argv."""
    config = os.path.join(session, "hyprland.lua")
    words = [executable, *args]
    command = " ".join(shlex.quote(word) for word in words)
    return ("#!/bin/bash\n"
            f"export HN_LIVE_OWNER_TOKEN=\"$(< {shlex.quote(session + '/owner-token')})\"\n"
            f"echo \"$BASHPID\" > {shlex.quote(scratch + '/hypr.pid')}\n"
            "exec env -u HYPRLAND_INSTANCE_SIGNATURE " + aq + " " + command +
            f" >{shlex.quote(scratch + '/hypr.log')} 2>&1\n")


def dispatch(path):
    return f"hl.dsp.exec_cmd({json.dumps(path)}, {{workspace=\"4 silent\"}})"


def main(argv):
    if argv[1] == "launcher":
        sys.stdout.write(launcher(argv[2], argv[3], argv[4]))
    elif argv[1] == "nested-launcher":
        # scratch, session, executable, aq, then remaining exact argv (argv[0] is executable)
        sys.stdout.write(nested_launcher(argv[2], argv[3], argv[4], argv[6:], argv[5]))
    elif argv[1] == "dispatch":
        sys.stdout.write(dispatch(argv[2]))
    else:
        raise SystemExit("usage: command_builders.py launcher DIR TOKEN AQ | nested-launcher SCRATCH SESSION EXE AQ ARGV... | dispatch PATH")


if __name__ == "__main__":
    main(sys.argv)
