# Encrypted archive build procedure

The repository is safe to publish because all generated secrets and hashes are
reset to fail-closed placeholders. Never commit the generated form of
`src/aes_rpa.c` or a `.archive_key` file.

## Private build steps

Use the same Python version that will be embedded in the final runtime.

1. Generate a per-game AES-256 key:

   ```text
   python scripts/generate_key.py path/to/project/game
   ```

2. Inject that key into the private build tree using the command printed by
   `generate_key.py`.

3. Generate the bytecode whitelist and loader hashes:

   ```text
   python scripts/inject_code_hash.py src/aes_rpa.c renpy path/to/project/game
   ```

4. On Windows, after the launcher executable is final, inject its hash:

   ```text
   python scripts/inject_exe_hash.py src/aes_rpa.c path/to/final/renpy.exe
   ```

5. Compile the engine with the companion `renpy-build` repository.

6. Before committing or publishing, restore the safe placeholders:

   ```text
   python scripts/reset_generated_security_data.py
   ```

Run `git diff -- src/aes_rpa.c` after step 6 and verify that it contains no
project-specific filenames, keys, executable hashes, or generated tables.
