#!/usr/bin/env python3
# Copyright 2026 The Ren'Py fork contributors
# SPDX-License-Identifier: MIT

"""
inject_exe_hash.py — Compute the SHA-256 of the finalized EXE (after Steam DRM),
then inject it as per-byte code immediates into aes_rpa.c's verify_launcher().

Usage:
    python inject_exe_hash.py <aes_rpa.c> <renpy.exe>

The script replaces the placeholder 0x00 values between the
    // HASH_COMPARE_BEGIN
    ...
    // HASH_COMPARE_END
markers with the actual SHA-256 bytes.

These values become code immediates (not .rdata references), so VMProtect
virtualization will embed them into the VM instruction stream where they
cannot be searched or replaced by a hex editor.

Build order:
    1. Compile renpy.exe
    2. Apply Steam DRM to renpy.exe      (EXE finalized)
    3. Run this script                    (inject hash into C source)
    4. Compile librenpython.dll           (hash is code immediates)
    5. Apply VMProtect to librenpython.dll (hash becomes VM bytecode)
"""

import hashlib
import sys
import re
import os


def compute_file_sha256(filepath):
    """Compute the SHA-256 hash of a file."""
    h = hashlib.sha256()
    with open(filepath, "rb") as f:
        while True:
            chunk = f.read(8192)
            if not chunk:
                break
            h.update(chunk)
    return h.digest()


def inject_exe_hash(c_source, exe_path):
    """
    Compute EXE's SHA-256 and inject it into the C source file.

    The hash bytes replace the per-byte comparison placeholders
    between HASH_COMPARE_BEGIN and HASH_COMPARE_END markers.
    """
    if not os.path.isfile(exe_path):
        print(f"ERROR: EXE not found: {exe_path}", file=sys.stderr)
        sys.exit(1)

    if not os.path.isfile(c_source):
        print(f"ERROR: C source not found: {c_source}", file=sys.stderr)
        sys.exit(1)

    hash_bytes = compute_file_sha256(exe_path)
    hash_hex = hash_bytes.hex()
    print(f"EXE path:   {exe_path}")
    print(f"EXE SHA-256: {hash_hex}")
    print(f"EXE size:    {os.path.getsize(exe_path)} bytes")

    # Generate 32 lines of per-byte comparison code
    comparisons = []
    for i, b in enumerate(hash_bytes):
        comparisons.append(f"    ok &= (hash[{i:2d}] == 0x{b:02x});")
    comparison_code = "\n".join(comparisons)

    with open(c_source, "r", encoding="utf-8") as f:
        content = f.read()

    # Verify the markers exist
    if "// HASH_COMPARE_BEGIN" not in content:
        print("ERROR: '// HASH_COMPARE_BEGIN' marker not found in C source.",
              file=sys.stderr)
        sys.exit(1)

    if "// HASH_COMPARE_END" not in content:
        print("ERROR: '// HASH_COMPARE_END' marker not found in C source.",
              file=sys.stderr)
        sys.exit(1)

    # Replace the region between markers
    pattern = r"(// HASH_COMPARE_BEGIN\n).*?(    // HASH_COMPARE_END)"
    replacement = rf"\g<1>{comparison_code}\n\g<2>"

    new_content, count = re.subn(pattern, replacement, content, flags=re.DOTALL)
    if count == 0:
        print("ERROR: Failed to replace hash comparison block. "
              "Check marker format.", file=sys.stderr)
        sys.exit(1)

    with open(c_source, "w", encoding="utf-8") as f:
        f.write(new_content)

    print(f"[OK] EXE hash injected into {c_source}")
    print(f"  Replaced {count} hash comparison block(s)")
    print()
    print("Next steps:")
    print("  1. Compile librenpython.dll (hash is now in verify_launcher())")
    print("  2. Apply VMProtect to librenpython.dll")
    print("     -> verify_launcher() will be virtualized")
    print("     -> hash bytes become unsearchable VM bytecode")


def main():
    if len(sys.argv) != 3:
        print("Usage: inject_exe_hash.py <aes_rpa.c> <renpy.exe>")
        print()
        print("Computes SHA-256 of <renpy.exe> and injects it as code")
        print("immediates into verify_launcher() in <aes_rpa.c>.")
        print()
        print("Example:")
        print("  python scripts/inject_exe_hash.py src/aes_rpa.c "
              "../../renpy-sdk/renpy.exe")
        sys.exit(1)

    inject_exe_hash(sys.argv[1], sys.argv[2])


if __name__ == "__main__":
    main()
