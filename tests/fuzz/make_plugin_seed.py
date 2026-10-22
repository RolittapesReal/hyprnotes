#!/usr/bin/env python3
"""Materialize compact tar corpus seed from checked-in example plugin fixture."""
from pathlib import Path
import tarfile

root = Path(__file__).resolve().parents[2]
source = root / "examples/plugins/word-count"
destination = Path(__file__).resolve().parent / "corpus/plugin_archive/valid.tar"
with tarfile.open(destination, "w", format=tarfile.USTAR_FORMAT) as archive:
    for name in ("plugin.json", "main.lua"):
        path = source / name
        info = archive.gettarinfo(str(path), arcname=f"word-count/{name}")
        info.mtime = 0
        with path.open("rb") as content:
            archive.addfile(info, content)
