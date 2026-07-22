#!/usr/bin/env python3
# Copyright 2026 The Ren'Py fork contributors
# SPDX-License-Identifier: MIT

"""
inject_aes_key.py - Inject AES-256 key into aes_rpa.c

Reads the key (as a 64-char hex string), generates a random XOR mask,
and writes per-byte `out[i] = obf ^ mask;` code immediates into the
recover_key() function.

After VMProtect virtualizes recover_key(), these immediate values
become part of the VM bytecode and cannot be found by searching
the binary's data sections.

Usage:
    python inject_aes_key.py <source.c> <key_hex>

Example:
    python inject_aes_key.py src/aes_rpa.c a1b2c3d4e5f6...  (64 hex chars)
"""

import os
import sys
import re


def inject_key(source_file, key_hex):
    """
    Inject the AES-256 key into the C source file as code immediates.

    key_hex: 64-character hexadecimal string (32 bytes)
    """
    key = bytes.fromhex(key_hex)
    assert len(key) == 32, "Key must be 32 bytes (64 hex chars)"

    # Generate a random mask
    mask = os.urandom(32)

    # Obfuscate: obfuscated = key XOR mask
    obfuscated = bytes(k ^ m for k, m in zip(key, mask))

    # Generate 32 lines of per-byte XOR code immediates.
    # Format: `m = 0xMM; out[i] = 0xOO ^ m;`
    # The volatile variable `m` in recover_key() prevents constant folding.
    lines = []
    for i in range(32):
        lines.append(f"    m = 0x{mask[i]:02x}; out[{i:2d}] = 0x{obfuscated[i]:02x} ^ m;")
    recovery_code = "\n".join(lines)

    with open(source_file, "r", encoding="utf-8") as f:
        content = f.read()

    # Replace the region between KEY_RECOVER_BEGIN and KEY_RECOVER_END
    pattern = r'(    // KEY_RECOVER_BEGIN\n).*?(    // KEY_RECOVER_END)'
    replacement = f'\\1{recovery_code}\n\\2'
    new_content = re.sub(pattern, replacement, content, flags=re.DOTALL)

    new_content, macro_count = re.subn(
        r"#define AES_KEY_INJECTED [01]",
        "#define AES_KEY_INJECTED 1",
        new_content,
        count=1,
    )

    if macro_count != 1:
        print("ERROR: AES_KEY_INJECTED marker not found!")
        sys.exit(1)

    if new_content == content:
        print("ERROR: KEY_RECOVER_BEGIN/END markers not found in source file!")
        sys.exit(1)

    with open(source_file, "w", encoding="utf-8") as f:
        f.write(new_content)

    print(f"Key injected into {source_file}")
    print(f"  Obfuscated key: {obfuscated.hex()}")
    print(f"  Mask:           {mask.hex()}")
    print(f"  Verification:   {bytes(o ^ m for o, m in zip(obfuscated, mask)).hex()}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: inject_aes_key.py <source.c> <key_hex>")
        print("  source.c: Path to aes_rpa.c")
        print("  key_hex:  64-character hex string (32 bytes)")
        sys.exit(1)

    inject_key(sys.argv[1], sys.argv[2])
