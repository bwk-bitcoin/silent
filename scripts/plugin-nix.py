#!/usr/bin/env python3
"""Run a plugin flake against filtered working-tree sources, without changing locks."""

import argparse
import os
import pathlib
import shutil
import subprocess
import tempfile


def snapshot(source, destination):
    files = subprocess.check_output([
        "git", "-C", str(source), "ls-files", "-z",
        "--cached", "--others", "--exclude-standard",
    ]).split(b"\0")
    destination.mkdir()
    for name in sorted(set(files) - {b""}):
        relative = pathlib.Path(os.fsdecode(name))
        origin = source / relative
        if not origin.is_file() and not origin.is_symlink():
            continue
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(origin, target, follow_symlinks=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("build", "eval"))
    parser.add_argument("attribute")
    parser.add_argument("options", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    host = pathlib.Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="silent-plugin-") as temporary:
        root = pathlib.Path(temporary)
        snapshot(host, root / "silent")
        snapshot(pathlib.Path.cwd(), root / "plugin")
        return subprocess.call([
            os.environ.get("NIX", "nix"),
            "--extra-experimental-features", "nix-command flakes",
            args.command, f"path:{root / 'plugin'}#{args.attribute}",
            "--override-input", "silent", f"path:{root / 'silent'}",
            "--no-write-lock-file", *args.options,
        ])


if __name__ == "__main__":
    raise SystemExit(main())
