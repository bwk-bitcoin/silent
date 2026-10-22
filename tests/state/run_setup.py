#!/usr/bin/env python3
"""Link the real built GUI with a fake device transport. Run each case in fresh HOME."""

import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    repo = Path(__file__).resolve().parents[2]
    build = repo / "build"
    database = json.loads((build / "compile_commands.json").read_text())
    entry = next(item for item in database if item["file"].endswith("/src/main.cpp"))
    test_flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "Qt6Test"], text=True))
    test_libraries = shlex.split(subprocess.check_output(["pkg-config", "--libs", "Qt6Test"], text=True))
    with tempfile.TemporaryDirectory(prefix="silent-setup-") as directory:
        scratch = Path(directory)
        objects = []
        for source in ("setup.cpp", "FakeUnlockDevices.cpp"):
            command = shlex.split(entry["command"])
            output = scratch / (source + ".o")
            command[command.index("-o") + 1] = str(output)
            command[-1] = str(repo / "tests/state" / source)
            command[1:1] = test_flags
            subprocess.run(command, cwd=entry["directory"], check=True)
            objects.append(str(output))
        command = shlex.split((build / "CMakeFiles/silent.dir/link.txt").read_text())
        command = [item for item in command if not item.endswith(("/src/main.cpp.o", "/src/state/UnlockDevices.cpp.o"))]
        command[command.index("-o") + 1] = str(scratch / "setup-test")
        command[1:1] = objects
        command.extend(test_libraries)
        subprocess.run(command, cwd=build, check=True)
        environment = dict(os.environ, QT_QPA_PLATFORM="offscreen")
        for mode in ("lifecycle", "password", "password-failure", "password-existing", "password-destroy", "device", "device-destroy", "screenshots"):
            home = scratch / mode
            home.mkdir()
            environment["HOME"] = str(home)
            arguments = [str(scratch / "setup-test"), mode]
            if mode == "screenshots":
                shots = Path(os.environ.get("SETUP_SHOTS", str(scratch / "shots")))
                shots.mkdir(parents=True, exist_ok=True)
                arguments.append(str(shots))
            subprocess.run(arguments, env=environment, check=True, timeout=120)


if __name__ == "__main__":
    main()
