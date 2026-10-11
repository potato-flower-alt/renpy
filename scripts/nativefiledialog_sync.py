"""Stage or install the desktop file dialog update in an existing SDK.

Staging preserves SDK extensions. Installation uses the verified staged files,
backs up every replaced file, and records hashes. Run with Windows Python.
"""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


PYTHON_FILES = (
    "renpy/__init__.py",
    "renpy/display/behavior.py",
    "renpy/display/core.py",
    "renpy/exports/__init__.py",
    "renpy/loadsave.py",
    "renpy/main.py",
    "renpy/rollback.py",
    "renpy/nativefiledialog.py",
)
RUNTIMES = {
    "py3-windows-x86_64": ("librenpython.dll", "libpython3.12.dll", "python.exe", "pythonw.exe", "renpy.exe"),
    "py3-linux-x86_64": ("librenpython.so", "python", "pythonw", "renpy"),
    "py3-linux-aarch64": ("librenpython.so", "python", "pythonw", "renpy"),
    "py3-mac-universal": ("librenpython.dylib", "python", "pythonw", "renpy"),
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("stage", "install"))
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--runtime-source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source, sdk, runtime, output = (p.resolve() for p in (args.source, args.sdk, args.runtime_source, args.output))
    stage = output / "staging-sdk"
    manifest_path = output / "sdk-manifest.json"
    if args.mode == "stage":
        (output / "staging-verified.json").unlink(missing_ok=True)
        stage.mkdir(parents=True, exist_ok=True)
        for directory in ("renpy", "lib", "renpy.app"):
            shutil.copytree(sdk / directory, stage / directory, dirs_exist_ok=True)
        for name in ("renpy.py", "renpy.sh", "renpy.exe"):
            shutil.copy2(sdk / name, stage / name)
        files = list(PYTHON_FILES)
        for relative in PYTHON_FILES:
            original = sdk / relative
            proposed = source / relative
            if original.exists():
                baseline = subprocess.check_output(["git", "show", "HEAD:" + relative], cwd=source)
                current = original.read_bytes().replace(b"\r\n", b"\n")
                if current not in (baseline.replace(b"\r\n", b"\n"), proposed.read_bytes().replace(b"\r\n", b"\n")):
                    raise SystemExit("SDK file has independent changes: " + relative)
            shutil.copy2(proposed, stage / relative)
        for platform, names in RUNTIMES.items():
            for name in names:
                relative = "lib/{}/{}".format(platform, name)
                origin = runtime / relative
                if not origin.is_file():
                    raise SystemExit("Missing built runtime: " + str(origin))
                shutil.copy2(origin, stage / relative)
                files.append(relative)
        shutil.copy2(runtime / "lib/py3-windows-x86_64/renpy.exe", stage / "renpy.exe")
        files.append("renpy.exe")
        for name in RUNTIMES["py3-mac-universal"]:
            relative = "renpy.app/Contents/MacOS/" + name
            shutil.copy2(stage / "lib/py3-mac-universal" / name, stage / relative)
            files.append(relative)
        # Ship the native source as reference alongside the executable SDK.
        for relative in (
            "renpy/_nativefiledialog.pyx",
            "src/nativefiledialog.cc",
            "src/nativefiledialog.h",
            "src/nativefiledialog_mac.mm",
            "src/Setup.nativefiledialog",
        ):
            target = stage / "nativefiledialog-source" / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source / relative, target)
            files.append("nativefiledialog-source/" + relative)
        target = stage / "doc/nativefiledialog.rst"
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source / "sphinx/source/nativefiledialog.rst", target)
        files.append("doc/nativefiledialog.rst")
        relative = "nativefiledialog-demo/game/script.rpy"
        target = stage / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source / "examples/nativefiledialog/game/script.rpy", target)
        files.append(relative)
        # Bundle signing updates these existing resource envelopes as well.
        for relative in ("renpy.app/Contents/_CodeSignature/CodeResources", "renpy.app/Contents/CodeResources"):
            if (stage / relative).exists():
                files.append(relative)
        manifest = {
            "source_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source).decode().strip(),
            "sdk": str(sdk),
            "stage": str(stage),
            "files": sorted(files),
            "installed": False,
            "previous_hashes": {
                relative: sha(sdk / relative) if (sdk / relative).is_file() else None for relative in files
            },
            "source_hashes": {
                relative: sha(source / relative)
                for relative in (
                    *PYTHON_FILES,
                    "renpy/_nativefiledialog.pyx",
                    "src/nativefiledialog.cc",
                    "src/nativefiledialog.h",
                    "src/nativefiledialog_mac.mm",
                )
            },
        }
        manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        print("Staged", len(files), "feature files in", stage)
    else:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        records = []
        # All testing and signing must precede installation.
        verification_path = output / "staging-verified.json"
        if not verification_path.is_file():
            raise SystemExit("Missing staging verification record.")
        verification = json.loads(verification_path.read_text(encoding="utf-8"))
        if verification.get("manifest_sha256") != sha(manifest_path):
            raise SystemExit("The SDK manifest changed after verification.")
        if set(verification.get("files", {})) != set(manifest["files"]):
            raise SystemExit("The verified files do not match the SDK manifest.")
        # Preflight every file before replacing any SDK content.
        for relative in manifest["files"]:
            if sha(stage / relative) != verification["files"][relative]:
                raise SystemExit("Staged file changed after verification: " + relative)
            old_hash = sha(sdk / relative) if (sdk / relative).is_file() else None
            if old_hash != manifest["previous_hashes"][relative]:
                raise SystemExit("SDK file changed since staging: " + relative)
        for relative in manifest["files"]:
            staged, destination = stage / relative, sdk / relative
            backup = output / "sdk-backup" / relative
            old_hash = sha(destination) if destination.exists() else None
            if destination.exists() and not backup.exists():
                backup.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(destination, backup)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(staged, destination)
            records.append({"path": relative, "sha256": sha(destination), "previous_sha256": old_hash})
        manifest["installed"] = True
        manifest["records"] = records
        manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        shutil.copy2(manifest_path, sdk / "NATIVEFILEDIALOG_BUILD.json")
        print("Installed", len(records), "verified files; backup:", output / "sdk-backup")


if __name__ == "__main__":
    main()
