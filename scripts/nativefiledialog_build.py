"""Sync the local engine into an existing WSL build tree and build desktop runtimes.

Run inside Ubuntu WSL with its existing build Python:
  python nativefiledialog_build.py --source /mnt/i/G/Code/renpy/renpy \
      --build-root /root/renpy-build --output /mnt/i/G/Code/renpy/output/nativefiledialog
No SDK files are changed by this script. Changed WSL source files are backed up.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def digest(data):
    return hashlib.sha256(data.replace(b"\r\n", b"\n")).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--platforms", default="windows,linux,mac")
    args = parser.parse_args()
    source, build_root, output = args.source.resolve(), args.build_root.resolve(), args.output.resolve()
    if not (build_root / "renpybuild" / "__main__.py").is_file():
        raise SystemExit("An existing prepared Ren'Py build tree is required.")
    if not (source / "src" / "nativefiledialog.cc").is_file():
        raise SystemExit("The source tree does not contain the native file dialog implementation.")
    output.mkdir(parents=True, exist_ok=True)
    target = build_root / "renpy"
    files = subprocess.check_output(["git", "ls-files", "-z"], cwd=source).decode().split("\0")
    # Include the new feature's untracked sources, but not unrelated untracked files.
    files += [
        "renpy/nativefiledialog.py",
        "renpy/_nativefiledialog.pyx",
        "src/nativefiledialog.h",
        "src/nativefiledialog.cc",
        "src/nativefiledialog_mac.mm",
        "src/Setup.nativefiledialog",
        "unittests/test_nativefiledialog.py",
        "sphinx/source/nativefiledialog.rst",
    ]
    records = []
    for relative in sorted(set(files)):
        if not relative:
            continue
        origin, destination = source / relative, target / relative
        if not origin.is_file():
            continue
        data = origin.read_bytes()
        old = destination.read_bytes() if destination.is_file() else None
        if old is not None and digest(data) == digest(old):
            continue
        if old is not None:
            backup = output / "wsl-source-backup" / relative
            if not backup.exists():
                backup.parent.mkdir(parents=True, exist_ok=True)
                backup.write_bytes(old)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data.replace(b"\r\n", b"\n"))
        records.append({
            "path": relative,
            "sha256": digest(data),
            "previous_sha256": digest(old) if old is not None else None,
        })
    # Apply only the native additions to the possibly customised build task.
    origin = source.parent / "renpy-build" / "tasks" / "librenpy.py"
    destination = build_root / "tasks" / "librenpy.py"
    data = origin.read_bytes()
    old = destination.read_bytes()
    backup = output / "wsl-source-backup" / "build-tasks" / "librenpy.py"
    if not backup.exists():
        backup.parent.mkdir(parents=True, exist_ok=True)
        backup.write_bytes(old)
    # The baseline build task must match before replacing it. Ignore line endings.
    baseline = subprocess.check_output(["git", "show", "HEAD:tasks/librenpy.py"], cwd=origin.parent.parent)
    previous = output / "wsl-build-task-sha256.txt"
    accepted = {digest(baseline), digest(data)}
    # Accept the earlier native-only header-path variant during an incremental
    # rebuild, without accepting unrelated edits to this build task.
    accepted.add(digest(data.replace(b'/dbus-1.0/include"', b'/dbus-1.0/include -I{{ sysroot }}/usr/include"')))
    if previous.exists():
        accepted.add(previous.read_text().strip())
    if digest(old) not in accepted:
        raise SystemExit("The WSL librenpy task has independent changes; merge the native additions first.")
    destination.write_bytes(data.replace(b"\r\n", b"\n"))
    previous.write_text(digest(data))
    manifest = {
        "source_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source).decode().strip(),
        "source_changes": records,
        "builds": [],
        "source_hashes": {
            relative: digest((source / relative).read_bytes())
            for relative in sorted(set(files))
            if relative and (source / relative).is_file()
        },
        "build_task_sha256": digest(data),
    }
    environment = os.environ.copy()
    environment["PATH"] = str(Path(sys.executable).parent) + os.pathsep + environment.get("PATH", "")
    environment["PYTHONHASHSEED"] = "0"
    for platform in args.platforms.split(","):
        if platform not in ("windows", "linux", "mac"):
            raise SystemExit("Unsupported desktop build platform: " + platform)
        command = [
            sys.executable,
            "-m",
            "renpybuild",
            "--platform",
            platform,
            "--python",
            "3",
            "rebuild",
            "librenpy",
            "renpython",
        ]
        log = output / ("build-" + platform + ".log")
        print("Building", platform, "->", log, flush=True)
        with log.open("wb") as stream:
            result = subprocess.run(command, cwd=build_root, stdout=stream, stderr=subprocess.STDOUT, env=environment)
        manifest["builds"].append({"platform": platform, "command": command, "exit_code": result.returncode})
        (output / "build-manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        if result.returncode:
            raise SystemExit("Build failed; see " + str(log))
    print("All requested desktop builds completed.", flush=True)


if __name__ == "__main__":
    main()
