#!/usr/bin/env python3
# Copyright 2026 The Ren'Py fork contributors
# SPDX-License-Identifier: MIT

"""
inject_code_hash.py — Build-time bytecode hash injection (extended for Gate 6)

Compiles all relevant Ren'Py engine modules and game scripts with the target
Python interpreter, extracts the bytecode of EVERY function (not just a few),
computes their FNV-1a 32-bit hashes, and injects a static whitelist table into
aes_rpa.c between the CODE_WHITELIST_BEGIN / CODE_WHITELIST_END markers.

This enables Gate 6: full-stack bytecode integrity verification — every frame
in the Python call stack during decryption must have its (filename, funcname,
bytecode) match an entry in this embedded table.

Usage:
    python inject_code_hash.py <aes_rpa.c> <renpy_dir> [game_dir]

    renpy_dir: Path to the renpy/ package (e.g., renpy/)
    game_dir:  Optional path to the game/ scripts directory

IMPORTANT: This script MUST be run using the same Python version that
will be embedded in the final build (currently Python 3.12).  Different
Python versions produce different bytecode, resulting in different hashes.

The injected table becomes part of verify_python_caller()'s static data.
After VMProtect processes the DLL, the function is protected by mutation.
"""

import os
import re
import sys
import types


def fnv1a_32(data: bytes) -> int:
    """Compute FNV-1a 32-bit hash over bytes."""
    h = 2166136261
    for b in data:
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def fnv1a_32_str(s: str) -> int:
    """Compute FNV-1a 32-bit hash over a string (encoded as UTF-8)."""
    return fnv1a_32(s.encode("utf-8"))


def collect_code_objects(code: types.CodeType, filename: str, collected: list):
    """
    Recursively collect all code objects (functions, lambdas, comprehensions,
    class bodies, module-level) from a module's compiled code tree.

    collected: list of (filename, funcname, bytecode_hash) tuples.
               Duplicate (filename, funcname) entries are ALLOWED — Gate 6
               will accept a frame if its bytecode matches ANY entry with
               the same (filename, funcname). This is needed because .rpy
               files have multiple Python blocks with co_name = "<module>".
    """
    collected.append((filename, code.co_name, fnv1a_32(code.co_code)))

    for const in code.co_consts:
        if isinstance(const, types.CodeType):
            collect_code_objects(const, filename, collected)


def collect_from_py_file(filepath: str, relpath: str, collected: list):
    """
    Compile a .py file and collect all code objects.
    relpath is the path stored in co_filename (used as the lookup key at runtime).
    """
    with open(filepath, "r", encoding="utf-8") as f:
        source = f.read()

    try:
        module_code = compile(source, relpath, "exec")
    except SyntaxError as e:
        print(f"  WARNING: Skipping {relpath} — syntax error: {e}")
        return

    collect_code_objects(module_code, relpath, collected)
    # Count entries from this file only
    file_entries = sum(1 for e in collected if e[0] == relpath)
    print(f"  {relpath}: {file_entries} functions")


def collect_from_rpy_file(filepath: str, relpath: str, collected: list):
    """
    Compile a .rpy file's Python blocks and collect all code objects.

    .rpy files contain Ren'Py script with embedded Python blocks
    (init python:, python hide:, etc.). We strip Ren'Py-specific syntax
    and compile the embedded Python. Since Ren'Py's parser handles the
    actual compilation, we do our best to extract Python blocks here.
    """
    with open(filepath, "r", encoding="utf-8") as f:
        source = f.read()

    # Strategy: extract Python blocks from .rpy files.
    # Ren'Py uses "init python:" / "python:" / "python hide:" / "python early:"
    # blocks. We extract the indented block content and compile each one.
    python_blocks = []
    lines = source.split("\n")
    i = 0
    while i < len(lines):
        line = lines[i].strip()
        # Match "python ...:" or "init ... python ...:" patterns
        is_python_block = False
        if line.startswith("python") and (line.endswith(":") or ":" in line):
            is_python_block = True
        elif "python" in line and line.rstrip().endswith(":") and (
            line.startswith("init") or line.startswith("early")
        ):
            is_python_block = True

        if is_python_block:
            # Find the indented block
            i += 1
            block_lines = []
            while i < len(lines):
                if lines[i].strip() == "":
                    block_lines.append("")
                    i += 1
                    continue
                # Check if dedented (not indented relative to block start)
                if lines[i] and not lines[i][0].isspace() and lines[i].strip():
                    break
                block_lines.append(lines[i])
                i += 1

            if block_lines:
                # Determine base indentation
                non_empty = [line for line in block_lines if line.strip()]
                if non_empty:
                    base_indent = len(non_empty[0]) - len(non_empty[0].lstrip())
                    dedented = []
                    for line in block_lines:
                        if line.strip():
                            dedented.append(line[base_indent:] if len(line) >= base_indent else line)
                        else:
                            dedented.append("")
                    python_blocks.append("\n".join(dedented))
            continue
        i += 1

    # Compile each Python block.
    # IMPORTANT: All blocks from the same .rpy file must use the SAME
    # co_filename (just `relpath`, not `relpath:block_N`).  Ren'Py's
    # py_compile() uses the .rpy file path as the filename for every
    # Python block in that file.  Using different filenames would cause
    # Gate 6 lookups to miss at runtime.
    block_count = 0
    for j, block_source in enumerate(python_blocks):
        if not block_source.strip():
            continue
        try:
            block_code = compile(block_source, relpath, "exec")
            collect_code_objects(block_code, relpath, collected)
            block_count += 1
        except SyntaxError:
            # Not all Python-looking blocks are valid standalone Python
            pass

    # Also try to compile any Python lines outside blocks (e.g. `define`, `default`)
    # These use Ren'Py syntax, not Python, so we skip them.

    print(f"  {relpath}: {block_count} python blocks → "
          f"{sum(1 for e in collected if e[0] == relpath)} functions")


def collect_from_directory(dirpath: str, prefix: str, collected: list, extensions: tuple):
    """
    Walk a directory, compile all files matching the given extensions,
    and collect their code objects.

    `prefix` is prepended to the relative path for the co_filename key
    (e.g. "renpy/" for engine modules).
    """
    count = 0
    for root, dirs, files in os.walk(dirpath):
        # Skip __pycache__ and hidden directories
        dirs[:] = [d for d in dirs if not d.startswith(".") and d != "__pycache__"]

        for fn in sorted(files):
            if not fn.endswith(extensions):
                continue

            filepath = os.path.join(root, fn)
            rel = os.path.relpath(filepath, dirpath).replace("\\", "/")

            # For the lookup key, use the path as it would appear in co_filename
            relpath = prefix + rel

            if fn.endswith(".py"):
                collect_from_py_file(filepath, relpath, collected)
            else:
                collect_from_rpy_file(filepath, relpath, collected)
            count += 1

    return count


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <aes_rpa.c> <renpy_dir> [game_dir] [extra_py ...]")
        print(f"Example: {sys.argv[0]} src/aes_rpa.c renpy/")
        print(f"         {sys.argv[0]} src/aes_rpa.c renpy/ game/")
        print(f"         {sys.argv[0]} src/aes_rpa.c renpy/ game/ game_launcher.py")
        sys.exit(1)

    c_source_path = sys.argv[1]
    renpy_dir = os.path.abspath(sys.argv[2])
    game_dir = os.path.abspath(sys.argv[3]) if len(sys.argv) >= 4 and os.path.isdir(sys.argv[3]) else None
    extra_start = 4 if game_dir else 3

    collected = []  # list of (filename, funcname, bytecode_hash) — duplicates allowed

    # ── Step 1: Collect from renpy/ engine modules (.py files) ──────────
    renpy_py_dir = renpy_dir
    print("Collecting from renpy/ engine .py files...")
    n = collect_from_directory(renpy_py_dir, "renpy/", collected, (".py",))
    print(f"  Total: {n} .py files")

    # ── Step 2: Collect from renpy/common/ (.rpy files) ─────────────────
    common_dir = os.path.join(renpy_dir, "common")
    if os.path.isdir(common_dir):
        print("\nCollecting from renpy/common/ .rpy files...")
        n = collect_from_directory(common_dir, "renpy/common/", collected, (".rpy",))
        print(f"  Total: {n} .rpy files")

    # ── Step 3: Collect from game/ (.rpy, .rpym, _ren.py files) ────────
    if game_dir and os.path.isdir(game_dir):
        print("\nCollecting from game/ scripts...")
        n = collect_from_directory(game_dir, "game/", collected, (".rpy", ".rpym", "_ren.py"))
        print(f"  Total: {n} script files")

    # ── Step 4: Collect from extra .py files (launcher scripts, etc.) ──
    for i in range(extra_start, len(sys.argv)):
        filepath = os.path.abspath(sys.argv[i])
        if not os.path.isfile(filepath):
            print(f"  WARNING: extra file not found: {filepath}")
            continue
        # Store with just the filename (no prefix) — Gate 6 path
        # normalization will match the suffix containing the filename
        fn = os.path.basename(filepath)
        print(f"\nCollecting from extra file: {fn}...")
        collect_from_py_file(filepath, fn, collected)

    total_funcs = len(collected)
    print(f"\nTotal function bytecodes collected: {total_funcs}")

    if total_funcs == 0:
        print("ERROR: No functions collected!")
        sys.exit(1)

    # ── Step 4: Generate whitelist table entries ────────────────────────
    # Sort by filename then funcname for deterministic output.
    # Duplicate (filename, funcname) entries are KEPT — Gate 6 checks all
    # matching entries and accepts if ANY bytecode hash matches.
    entries = sorted(collected, key=lambda x: (x[0], x[1]))

    indent = " " * 4
    lines = []
    lines.append(f"{indent}// CODE_WHITELIST_BEGIN")
    lines.append(f"{indent}// Auto-generated by inject_code_hash.py — do not edit by hand")
    lines.append(f"{indent}// {total_funcs} functions from Ren'Py engine and game scripts")
    lines.append(f"{indent}// Format: {{filename_hash, funcname_hash, bytecode_hash}}")

    for filename, funcname, bytecode_hash in entries:
        fn_hash = fnv1a_32_str(filename)
        name_hash = fnv1a_32_str(funcname)
        lines.append(
            f"{indent}{{0x{fn_hash:08x}u, 0x{name_hash:08x}u, 0x{bytecode_hash:08x}u}},"
            f"  // {filename}:{funcname}"
        )

    lines.append(f"{indent}// CODE_WHITELIST_END")
    replacement = "\n".join(lines)

    with open(c_source_path, "r", encoding="utf-8") as f:
        content = f.read()

    # Replace the CODE_WHITELIST_BEGIN/END region
    pattern = r"[ ]*// CODE_WHITELIST_BEGIN\n.*?[ ]*// CODE_WHITELIST_END"
    new_content, count = re.subn(pattern, replacement, content, flags=re.DOTALL)

    if count != 1:
        print(f"ERROR: Expected 1 CODE_WHITELIST marker region, found {count}")
        print("Make sure aes_rpa.c has the CODE_WHITELIST_BEGIN/END markers.")
        sys.exit(1)

    with open(c_source_path, "w", encoding="utf-8") as f:
        f.write(new_content)

    print(f"\nWhitelist injected into {c_source_path}")
    print(f"  {total_funcs} function hashes embedded in DLL code_whitelist table")

    print("\nInjecting Gate 4 hashes for loader.py functions...")
    _inject_gate4_hashes(c_source_path, renpy_dir)


def _inject_gate4_hashes(c_source_path: str, renpy_dir: str):
    """
    Find specific loader.py functions and inject their FNV-1a bytecode
    hashes into the CODE_HASH_BEGIN/END region (Gate 4).

    These are the 5 verified functions in the decryption data path:
    - AESDecryptingReader.readinto()
    - RPAEv2ArchiveHandler.read_index()
    - load_from_archive()
    - load_core()
    - load()
    """
    loader_path = os.path.join(renpy_dir, "loader.py")
    if not os.path.isfile(loader_path):
        print(f"  WARNING: {loader_path} not found, skipping Gate 4 hash injection")
        return

    with open(loader_path, "r", encoding="utf-8") as f:
        source = f.read()

    module_code = compile(source, "renpy/loader.py", "exec")

    def find_class_method(code: types.CodeType, class_name: str, func_name: str):
        """Recursively find a method inside a named class."""
        for const in code.co_consts:
            if not isinstance(const, types.CodeType):
                continue
            if const.co_name == class_name:
                for inner in const.co_consts:
                    if isinstance(inner, types.CodeType) and inner.co_name == func_name:
                        return inner
            result = find_class_method(const, class_name, func_name)
            if result is not None:
                return result
        return None

    def find_module_function(code: types.CodeType, func_name: str):
        """Find a top-level function in a module code object."""
        for const in code.co_consts:
            if isinstance(const, types.CodeType) and const.co_name == func_name:
                return const
        return None

    targets = [
        ("readinto",           "AESDecryptingReader",          "readinto"),
        ("read_index_v2",      "RPAEv2ArchiveHandler",         "read_index"),
        ("load_from_archive",  None,                           "load_from_archive"),
        ("load_core",          None,                           "load_core"),
        ("load",               None,                           "load"),
    ]

    hashes = {}
    for label, cls, func in targets:
        if cls is not None:
            co = find_class_method(module_code, cls, func)
            display_name = f"{cls}.{func}()"
        else:
            co = find_module_function(module_code, func)
            display_name = f"{func}()"

        if co is None:
            print(f"  ERROR: Could not find {display_name} in {loader_path}")
            sys.exit(1)

        h = fnv1a_32(co.co_code)
        hashes[label] = h
        print(f"  {display_name}:  bytecode={len(co.co_code)} bytes  FNV-1a=0x{h:08x}")

    # Inject into CODE_HASH_BEGIN/END
    with open(c_source_path, "r", encoding="utf-8") as f:
        content = f.read()

    indent = " " * 20
    lines = [
        f"{indent}// CODE_HASH_BEGIN",
        f'{indent}if (strcmp(funcname, "readinto") == 0)           '
        f'hash_ok = (h == 0x{hashes["readinto"]:08x});',
        f'{indent}if (strcmp(funcname, "read_index") == 0)         '
        f'hash_ok = (h == 0x{hashes["read_index_v2"]:08x});',
        f'{indent}if (strcmp(funcname, "load_from_archive") == 0)  '
        f'hash_ok = (h == 0x{hashes["load_from_archive"]:08x});',
        f'{indent}if (strcmp(funcname, "load_core") == 0)          '
        f'hash_ok = (h == 0x{hashes["load_core"]:08x});',
        f'{indent}if (strcmp(funcname, "load") == 0)               '
        f'hash_ok = (h == 0x{hashes["load"]:08x});',
        f"{indent}// CODE_HASH_END",
    ]
    replacement = "\n".join(lines)

    pattern = r"[ ]*// CODE_HASH_BEGIN\n.*?[ ]*// CODE_HASH_END"
    new_content, count = re.subn(pattern, replacement, content, flags=re.DOTALL)

    if count < 1:
        print(f"  ERROR: Expected at least 1 CODE_HASH marker region, found {count}")
        sys.exit(1)

    with open(c_source_path, "w", encoding="utf-8") as f:
        f.write(new_content)

    print(f"  Gate 4 hashes injected into {c_source_path}")


if __name__ == "__main__":
    main()
