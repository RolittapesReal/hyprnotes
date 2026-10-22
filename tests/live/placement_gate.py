#!/usr/bin/env python3
"""Read-only post-map gate; never dispatches a move or workspace switch."""
import json
import sys


def observed_workspace(clients, address, pid):
    matches = [c for c in clients if c.get("address") == address and c.get("pid") == int(pid)]
    if len(matches) != 1:
        return None
    workspace = matches[0].get("workspace")
    return workspace.get("id") if isinstance(workspace, dict) else None


def main(argv):
    rows = json.load(sys.stdin)
    value = observed_workspace(rows, argv[1], argv[2])
    if value is None:
        print("no exact address+PID client", file=sys.stderr)
        return 2
    print(value)
    return 0 if str(value) == "4" else 3


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
