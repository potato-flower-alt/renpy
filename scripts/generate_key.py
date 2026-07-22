#!/usr/bin/env python3
# Copyright 2026 The Ren'Py fork contributors
# SPDX-License-Identifier: MIT

"""
generate_key.py - Generate a random AES-256 encryption key

Generates a cryptographically secure random 32-byte key and writes it to
the game's `.archive_key` file (a dotfile that must NOT be committed to
version control or shipped in distributions).

The key must also be injected into the C layer via inject_aes_key.py
so the engine can decrypt archives at runtime.

Usage:
    python generate_key.py [game_directory]

Output:
    Writes the key (64-character hex string) to <game_directory>/.archive_key
    Prints the inject_aes_key.py command to run.
"""

import os
import sys


def generate_key(game_dir="game"):
    """Generate and write a random AES-256 key to the game's .archive_key file."""

    key = os.urandom(32)
    key_hex = key.hex()

    key_path = os.path.join(game_dir, ".archive_key")

    # Write the key to the dotfile
    os.makedirs(game_dir, exist_ok=True)
    with open(key_path, "w") as f:
        f.write(key_hex + "\n")

    print("=" * 70)
    print("AES-256 Encryption Key Generated")
    print("=" * 70)
    print()
    print(f"Key written to: {key_path}")
    print()
    print("Next steps:")
    print("  1. Inject the key into the C layer:")
    print(f"     python scripts/inject_aes_key.py src/aes_rpa.c {key_hex}")
    print()
    print("  2. Add to .gitignore:")
    print("     game/.archive_key")
    print()
    print("=" * 70)
    print("SECURITY WARNINGS:")
    print("  - NEVER commit game/.archive_key to version control!")
    print("  - NEVER ship .archive_key in distributions!")
    print("  - The key exists ONLY in this file and in the compiled engine.")
    print("  - If you lose this key, you CANNOT decrypt your archives.")
    print("  - Do NOT put the key in .rpy files — they are compiled to")
    print("    unprotected bytecode and shipped with the game.")
    print("=" * 70)

    return key_hex


if __name__ == "__main__":
    game_dir = sys.argv[1] if len(sys.argv) > 1 else "game"
    generate_key(game_dir)
