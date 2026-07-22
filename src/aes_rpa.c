// Copyright 2026 The Ren'Py fork contributors
// SPDX-License-Identifier: MIT
//
// src/aes_rpa.c
// RPA AES-256-CTR Encryption/Decryption Core
//
// This file implements the AES-256-CTR based encryption and decryption
// for Ren'Py's RPAE-2.0 encrypted archive format.
// It is compiled into librenpy.a -> librenpython.dll.
//
// OpenSSL (libcrypto) is already linked into librenpython.dll,
// so we can use EVP_* APIs directly without additional link changes.
//
// Anti-tamper:
//   - VMProtectIsValidImageCRC()   : DLL self-integrity (VMP built-in)
//   - verify_launcher()            : DLL verifies EXE hash
//     Hash bytes are code immediates inside a VMP-virtualized function,
//     so they cannot be searched/replaced in the .rdata section.

#include <string.h>
#include <stdlib.h>
#include <openssl/evp.h>
#include <Python.h>
#include <frameobject.h>  // PyFrameObject, PyFrame_GetCode, PyFrame_GetBack

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "advapi32.lib")
#endif

#ifdef USE_VMPROTECT
#include <stdbool.h>
#include "VMProtectSDK.h"
#endif

// Enable key cache to prevent the key from being stored in the .rdata section.
#define KEY_CACHE_EN 0

// Set to 1 by scripts/inject_aes_key.py. Public source deliberately ships
// without an embedded archive key.
#define AES_KEY_INJECTED 0

static void dlog(const char *fmt, ...) { (void)fmt; }



// ============================================================
// FNV-1a 32-bit hash helpers (used by Gate 4 and Gate 6)
// ============================================================

static inline unsigned int fnv1a_32_str(const char *s) {
    unsigned int h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

static inline unsigned int fnv1a_32_bytes(const unsigned char *data, Py_ssize_t len) {
    unsigned int h = 2166136261u;
    for (Py_ssize_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

// ============================================================
// Key Recovery (code immediates, not .rdata arrays)
// ============================================================
//
// The key is NOT stored as static arrays in .rdata (which VMProtect
// does not protect). Instead, each byte is recovered via XOR of two
// code immediates inside a VMP-virtualized function. After VMProtect
// processes the DLL, these immediate values become part of the VM
// bytecode and cannot be found by searching data sections.
//
// To prevent the compiler from constant-folding `obf ^ mask` at
// compile time (which would place the plain key bytes into .rdata),
// each mask byte is assigned through a `volatile` local variable.
// The `volatile` qualifier forces the compiler to treat each mask
// value as potentially changing at runtime, so the XOR must be
// executed as an actual instruction rather than being pre-computed.
//
// Build order:
//   1. inject_aes_key.py writes the XOR pairs as code immediates
//   2. Compiler embeds them as instruction operands in .text
//   3. VMProtect virtualizes recover_key() → values become VM bytecode
//
// __attribute__((noinline)) ensures this function is NOT inlined into
// aes_rpa_decrypt(), so it remains a distinct function that VMProtect
// can locate and protect.
static void __attribute__((noinline)) recover_key(unsigned char out[32]) {
#ifdef USE_VMPROTECT
    VMProtectBeginVirtualization("recover_key");
#endif

    // Each mask byte is loaded into a volatile local so the compiler
    // cannot fold `obfuscated ^ mask` into a constant. The volatile
    // read forces a runtime XOR instruction for each byte.
    //
    // These 0xXX values are replaced by inject_aes_key.py at build time.
    // After VMProtect virtualization, they become VM bytecode.
    volatile unsigned char m;
    // KEY_RECOVER_BEGIN
    memset(out, 0, 32); /* Build-time placeholder. */
    // KEY_RECOVER_END
    m = 0;

#ifdef USE_VMPROTECT
    VMProtectEnd();
#endif
}

// ============================================================
// Anti-Tamper: DLL verifies EXE hash (Windows only)
// ============================================================
//
// The EXE hash is NOT stored in .rdata (which VMP does not protect).
// Instead, each byte comparison is a code immediate inside a
// VMP-virtualized function. After VMProtect processes the DLL,
// these 0xXX values become part of the VM bytecode and cannot be
// located or replaced by a hex editor.
//
// Build order (critical!):
//   1. Compile renpy.exe
//   2. Apply Steam DRM to renpy.exe  (EXE content finalized)
//   3. Run inject_exe_hash.py: compute SHA-256, write into this file
//   4. Compile librenpython.dll (contains hash as code immediates)
//   5. Apply VMProtect to librenpython.dll
//      -> verify_launcher() is virtualized
//      -> hash bytes become VM bytecode, unsearchable

#ifdef _WIN32

// Compute SHA-256 of the main EXE (the process image).
// Uses Windows CryptoAPI (available on all supported Windows versions).
//
// Protected with VMProtect Mutation (not Virtualization) because:
// - This function makes many external Windows API calls
//   (CreateFileW, ReadFile, CryptHashData, etc.)
// - VMP cannot virtualize the API call sites themselves
// - Mutation still obfuscates the control flow, making it hard to
//   locate and hook/patch this function
// - Without protection, an attacker could patch this to return a
//   fake hash, completely bypassing verify_launcher()
static void __attribute__((noinline)) compute_exe_sha256(unsigned char out[32]) {
#ifdef USE_VMPROTECT
    VMProtectBeginMutation("compute_exe_sha256");
#endif

    wchar_t exe_path[4096];
    HANDLE hFile;
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    BYTE buffer[8192];
    DWORD bytesRead;
    DWORD hashLen = 32;

    memset(out, 0, 32);

    if (GetModuleFileNameW(NULL, exe_path, 4096) == 0)
        goto done;

    hFile = CreateFileW(exe_path, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        goto done;

    if (!CryptAcquireContextW(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        CloseHandle(hFile);
        goto done;
    }

    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        CloseHandle(hFile);
        goto done;
    }

    while (ReadFile(hFile, buffer, sizeof(buffer), &bytesRead, NULL) && bytesRead > 0) {
        if (!CryptHashData(hHash, buffer, bytesRead, 0)) {
            CryptDestroyHash(hHash);
            CryptReleaseContext(hProv, 0);
            CloseHandle(hFile);
            goto done;
        }
    }

    CryptGetHashParam(hHash, HP_HASHVAL, out, &hashLen, 0);

    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
    CloseHandle(hFile);

done:
#ifdef USE_VMPROTECT
    VMProtectEnd();
#endif
    return;
}

// Verify that the launcher EXE has not been tampered with.
//
// Each hash byte is compared as a code immediate (not a data reference),
// so VMProtect virtualization embeds them into the VM instruction stream.
//
// Result is cached: SHA-256 is computed only once per process lifetime.
// Returns 1 if EXE is intact, 0 if tampered or on error.
static int __attribute__((noinline)) verify_launcher(void) {
#ifdef USE_VMPROTECT
    VMProtectBeginVirtualization("verify_launcher");
#endif

    static int verified = -1;
    if (verified >= 0) {
#ifdef USE_VMPROTECT
        VMProtectEnd();
#endif
        return verified;
    }

    unsigned char hash[32];
    compute_exe_sha256(hash);

    // Per-byte comparison as code immediates.
    // These 0x00 placeholder values are replaced by inject_exe_hash.py
    // with the actual SHA-256 of the Steam-DRM-wrapped EXE.
    // After VMProtect virtualization, these become VM bytecode.
    int ok = 1;
    // HASH_COMPARE_BEGIN
    ok = 0; /* Build-time placeholder: fail closed. */
    // HASH_COMPARE_END

    // Clear hash from stack
    memset(hash, 0, 32);

    verified = ok;

#ifdef USE_VMPROTECT
    VMProtectEnd();
#endif
    return verified;
}

#endif /* _WIN32 */

// ============================================================
// Anti-Tamper: DLL self-integrity check (cached)
// ============================================================
//
// VMProtectIsValidImageCRC() scans the entire DLL memory image and
// computes a CRC — this is expensive (~1-5ms for a large DLL).
// Since aes_rpa_decrypt() is a hot path called on every resource read,
// we cache the result: the DLL image does not change after loading,
// so checking once per process lifetime is sufficient.
//
// This mirrors the caching pattern used by verify_launcher() above.

#ifdef USE_VMPROTECT
static int __attribute__((noinline)) check_dll_integrity(void) {
    VMProtectBeginVirtualization("check_dll_crc");

    static int crc_ok = -1;
    if (crc_ok >= 0) {
        VMProtectEnd();
        return crc_ok;
    }

    crc_ok = VMProtectIsValidImageCRC() ? 1 : 0;

    VMProtectEnd();
    return crc_ok;
}
#endif

// ============================================================
// Anti-Script-Injection: Python call stack verification
// ============================================================
//
// Prevents unauthorized Python code from calling decrypt_block().
// Walks the entire Python call stack and enforces five checks:
//
//   Gate 1 (all frames): Reject calls originating from interactive
//          sources — <console>, <string>, <stdin>.
//
//   Gate 2 (depth 0):    The direct Python caller must be readinto() or
//          read_index() in renpy/loader.py.
//
//   Gate 3 (depth 1~3):  Nearby frames must be from the Ren'Py engine
//          or Python stdlib — blocks monkey-patch wrapper attacks.
//
//   Gate 4 (ALL renpy/loader.py frames):  The bytecode of every known
//          loader.py function on the call stack must match the expected
//          FNV-1a hash.  This covers the entire plaintext data flow
//          path: readinto, read_index, load_from_archive, load_core,
//          and load.  If ANY of these functions is modified (e.g. to
//          add resource-dumping code), the bytecode changes and the
//          hash will not match.  Hash values are code immediates
//          injected by inject_code_hash.py at build time, then
//          protected by VMP.
//
//   Gate 5 (all frames): Every frame in the entire call stack must be
//          from renpy/, Python stdlib, or frozen/builtin modules.
//          Extends Gate 3 from depth 1~3 to ALL depths, blocking any
//          third-party or injected file from appearing anywhere in the
//          call chain.
//
//   Gate 6 (all frames, bytecode integrity): For every non-stdlib frame,
//          the bytecode FNV-1a hash (over co_code) must match the
//          build-time embedded CODE_WHITELIST table.  This catches:
//          - Injected .rpy/.py files (no table entry)
//          - Modified engine .py files (bytecode hash mismatch)
//          - Malicious .pyc substituted for original .py (co_code from .pyc)
//          Unlike Gate 5's filename-based whitelist, Gate 6 verifies
//          the actual bytecode being executed, so modifying any engine
//          file (even in renpy/) is detected.
//
// Performance optimization: After the first successful full verification
// of readinto/read_index, we cache their PyCodeObject pointers. On
// subsequent calls, we compare the depth-0 code object pointer against
// the cached ones — a single pointer comparison instead of a full stack
// walk. This is safe because:
//   - CPython code objects are immutable after creation
//   - Code object addresses are stable (not moved by GC)
//   - Monkey-patching creates a different code object → different pointer
//     → falls through to full verification → fails Gate 4 bytecode hash
//
// Protected with VMProtect Mutation (not Virtualization) because:
// - Many external Python C API calls that VMP cannot virtualize
// - Mutation still obfuscates control flow sufficiently
//
// Called at the very start of aes_rpa_decrypt(), before recover_key().
// Returns 1 if caller is legitimate, 0 otherwise.

// Cached PyCodeObject pointers for verified callers.
// Set after the first successful full verification. Not Py_INCREF'd —
// we only compare addresses, never dereference through these pointers
// on the fast path. The code objects are kept alive by the module's
// reference anyway (loader.py stays loaded for the process lifetime).
static PyCodeObject *verified_readinto_code = NULL;
static PyCodeObject *verified_read_index_code = NULL;

// ============================================================
// Trusted Game Script Registry (Runtime)
// ============================================================
//
// Populated at runtime when a .rpyc file is successfully loaded
// from an encrypted .rpa archive.  Gate 5 uses this set to
// distinguish legitimate game scripts (shipped by the developer
// inside encrypted archives) from attacker-injected .rpy files
// placed directly on disk.
//
// An attacker with disk write access to game/ could inject a
// malicious .rpy file, but cannot modify files inside an
// encrypted .rpa without the AES key.  Scripts loaded from
// inside encrypted archives are therefore trusted.

#define TRUSTED_SCRIPTS_MAX 1024

typedef struct {
    char *normalized_path;   // normalized path (malloc'd)
    unsigned int path_hash;  // FNV-1a hash of normalized path
} trusted_script_entry_t;

static trusted_script_entry_t trusted_scripts[TRUSTED_SCRIPTS_MAX];
static int trusted_scripts_count = 0;

// Normalize a script path for comparison.
// Returns a malloc'd string, or NULL on failure.
static char *normalize_script_path(const char *filename) {
    // Skip past "game/" or "game\" prefix if present
    const char *base = filename;
    const char *p;
    if ((p = strstr(filename, "game/")) != NULL ||
        (p = strstr(filename, "game\\")) != NULL) {
        base = p + 5;  // skip "game/" or "game\"
    }

    size_t len = strlen(base) + 1;
    char *result = (char *)malloc(len);
    if (!result) return NULL;

    // Copy with backslash→slash normalization; stop at .rpy/.rpyc/.rpyp
    size_t j = 0;
    for (const char *s = base; *s; s++) {
        char c = (*s == '\\') ? '/' : *s;
        if (c == '.' && j > 0) {
            if (strcmp(s, ".rpy") == 0 ||
                strcmp(s, ".rpyc") == 0 ||
                strcmp(s, ".rpyp") == 0) {
                break;
            }
        }
        result[j++] = c;
    }
    result[j] = '\0';
    return result;
}

// ── Forward declarations for register-path verification ──
// verify_python_caller_ex() and caller_context_t are defined later
// in this file (inside a VMProtect mutation region), but
// _c_register_trusted_script() needs to call them.

typedef enum {
    CALLER_DECRYPT = 0,
    CALLER_REGISTER = 1,
} caller_context_t;

static int verify_python_caller_ex(caller_context_t ctx);

// Called from Python (via aescrypt.pyx) after loading a .rpyc from
// an encrypted archive.  The filename is the archive index key,
// e.g. "game/scripts/screens/splashscreen.rpyc".
//
// SECURITY: Delegates to verify_python_caller() — the same full
// 6-gate call-stack verification that protects decrypt_block().
// Without this check, an attacker could inject a malicious .rpy
// that calls register_trusted_script() directly to whitelist itself,
// or modify loader.py bytecode to sneak past a naive caller check.
void _c_register_trusted_script(const char *filename) {
    if (trusted_scripts_count >= TRUSTED_SCRIPTS_MAX) return;
    if (!filename) return;

    // ── Caller verification ──────────────────────────────────
    // Reuses verify_python_caller_ex(CALLER_REGISTER) — the same
    // full-stack check that protects decrypt_block(), adapted for
    // the register call path:
    //   depth 0: aescrypt.register_trusted_script (compiled Cython)
    //   depth 1: loader.load_from_archive (Gate 4 bytecode hash)
    //   all depths: Gates 1, 3, 4, 5, 6
    // This ensures no script can be registered as trusted unless
    // the call originates from a verified loader.py chain.
    // Prevents:
    //   1. Attacker calling register_trusted_script() from their own code
    //   2. Attacker modifying load_from_archive's bytecode to register
    //      malicious scripts (caught by Gate 4 bytecode hash)
    //   3. Injected frames anywhere in the call stack (caught by Gates 5-6)
    if (!verify_python_caller_ex(CALLER_REGISTER)) return;

    char *normalized = normalize_script_path(filename);
    if (!normalized) return;

    // Deduplicate
    for (int i = 0; i < trusted_scripts_count; i++) {
        if (strcmp(trusted_scripts[i].normalized_path, normalized) == 0) {
            free(normalized);
            return;
        }
    }

    trusted_scripts[trusted_scripts_count].normalized_path = normalized;
    trusted_scripts[trusted_scripts_count].path_hash = fnv1a_32_str(normalized);
    trusted_scripts_count++;
}

// Called from Gate 5 (hot path).  Uses stack buffer — no heap allocation.
static int is_trusted_script(const char *filename) {
    if (!filename || trusted_scripts_count == 0) return 0;

    // Inline normalization into stack buffer
    const char *base = filename;
    const char *p;
    if ((p = strstr(filename, "game/")) != NULL ||
        (p = strstr(filename, "game\\")) != NULL) {
        base = p + 5;
    }

    char normalized[384];
    size_t j = 0;
    for (const char *s = base; *s && j < sizeof(normalized) - 1; s++) {
        char c = (*s == '\\') ? '/' : *s;
        if (c == '.' && j > 0) {
            if (strcmp(s, ".rpy") == 0 ||
                strcmp(s, ".rpyc") == 0 ||
                strcmp(s, ".rpyp") == 0) {
                break;
            }
        }
        normalized[j++] = c;
    }
    normalized[j] = '\0';

    unsigned int hash = fnv1a_32_str(normalized);
    for (int i = 0; i < trusted_scripts_count; i++) {
        if (trusted_scripts[i].path_hash == hash &&
            strcmp(trusted_scripts[i].normalized_path, normalized) == 0) {
            return 1;
        }
    }
    return 0;
}

// ============================================================
// Gate 6: Full-stack bytecode integrity whitelist
// ============================================================
//
// Each entry maps (filename, funcname) → expected bytecode FNV-1a hash.
// During decryption, every frame in the Python call stack that is NOT
// from stdlib/frozen/builtin MUST match an entry in this table with the
// correct bytecode hash.  This prevents:
//   - Injecting new .rpy/.py files (no table entry → rejected)
//   - Modifying any engine .py file (bytecode changes → hash mismatch)
//   - Replacing .py with malicious .pyc (co_code comes from .pyc → mismatch)
//
// Populated by inject_code_hash.py at build time.  The table values are
// code immediates — after VMProtect, they are part of the protected
// verify_python_caller() function's mutation region.

typedef struct {
    unsigned int filename_hash;   // FNV-1a of co_filename
    unsigned int funcname_hash;   // FNV-1a of co_name
    unsigned int bytecode_hash;   // FNV-1a of co_code
} bytecode_entry_t;

static const bytecode_entry_t BYTECODE_WHITELIST[] = {
    // CODE_WHITELIST_BEGIN
    // Generated by scripts/inject_code_hash.py for private builds.
    {0u, 0u, 0u}, /* Build-time placeholder: no caller matches. */
    // CODE_WHITELIST_END
};
static const int BYTECODE_WHITELIST_SIZE =
    sizeof(BYTECODE_WHITELIST) / sizeof(bytecode_entry_t);

static int __attribute__((noinline)) verify_python_caller_ex(caller_context_t ctx) {
#ifdef USE_VMPROTECT
    VMProtectBeginMutation("verify_caller");
#endif

    int ok = 0;
    int found_valid_entry = 0;

    PyFrameObject *frame = PyEval_GetFrame();  // borrowed ref
    if (!frame) {
        goto done;
    }

    // ── Fast path: check if depth-0 code object was already verified ──
    // Gates 1-5 can be skipped if depth-0 is a known-good caller.
    // But Gate 6 MUST ALWAYS run — it verifies the full call stack's
    // bytecode integrity, and injected frames can appear above a
    // legitimate depth-0 caller (e.g. readinto called from dump script).
    int gates_1_5_skip = 0;
    {
        PyCodeObject *fast_code = PyFrame_GetCode(frame);  // new ref
        if (fast_code) {
            if (fast_code == verified_readinto_code ||
                fast_code == verified_read_index_code) {
                gates_1_5_skip = 1;
            }
            Py_DECREF(fast_code);
        }
    }

    PyFrameObject *current = frame;
    Py_INCREF(current);

    int depth = 0;
    while (current != NULL && depth < 64) {  // cap to prevent infinite loop
        PyCodeObject *code = PyFrame_GetCode(current);  // new ref (3.9+)
        if (!code) {
            Py_DECREF(current);
            goto done;
        }

        const char *filename = PyUnicode_AsUTF8(code->co_filename);
        const char *funcname = PyUnicode_AsUTF8(code->co_name);

        if (!filename || !funcname) {
            Py_DECREF(code);
            Py_DECREF(current);
            goto done;
        }

        // ── Gates 1-4: Skippable via fast path ─────────────────────
        // When depth-0 was previously verified (gates_1_5_skip=1),
        // we skip these entry-point checks.  Gate 5 and Gate 6 must
        // ALWAYS run because the call stack above depth 0 may contain
        // injected frames that weren't present during the first full
        // verification.
        if (!gates_1_5_skip) {

        // ── Gate 1: Absolutely forbidden sources (full stack scan) ──
        if (strstr(filename, "<console>") ||   // Ren'Py developer console
            strstr(filename, "<string>")  ||   // eval()/exec()
            strstr(filename, "<stdin>"))  {    // interactive Python
            Py_DECREF(code);
            Py_DECREF(current);
            goto done;
        }

        // ── Gate 2: Entry point validation (depth 0) ──
        if (depth == 0) {
            if (ctx == CALLER_DECRYPT) {
                int is_loader = (strstr(filename, "renpy/loader") ||
                                 strstr(filename, "renpy\\loader"));
                int is_valid_func = (strcmp(funcname, "readinto") == 0 ||
                                     strcmp(funcname, "read_index") == 0);

                if (!is_loader || !is_valid_func) {
                    Py_DECREF(code);
                    Py_DECREF(current);
                    goto done;
                }
                found_valid_entry = 1;
            } else {  // CALLER_REGISTER
                // Depth 0 must be load_from_archive in renpy/loader.py.
                // (The Cython aescrypt.register_trusted_script wrapper is a
                // direct C call from Cython → C, so its Python frame may
                // not appear on the PyEval frame stack.)
                // Bytecode hash integrity is verified by Gate 4 below —
                // a modified load_from_archive will fail the hash check
                // against the build-time embedded value 0x77062154.
                int is_loader = (strstr(filename, "renpy/loader") ||
                                 strstr(filename, "renpy\\loader"));
                int is_lfa = (strcmp(funcname, "load_from_archive") == 0);
                if (!is_loader || !is_lfa) {
                    Py_DECREF(code);
                    Py_DECREF(current);
                    goto done;
                }
                found_valid_entry = 1;
            }
        }

        // ── Gate 4: Bytecode integrity verification (all loader.py frames) ──
        // For EVERY frame from renpy/loader.py, if the function name is
        // in the verified list, check that its bytecode FNV-1a hash
        // matches the expected value.  This covers the entire plaintext
        // data flow path, not just the direct caller (depth 0).
        //
        // The attack analysis showed that modifying load_from_archive()
        // (depth 1) could bypass the old depth-0-only Gate 4.  Now any
        // modification to readinto, read_index, load_from_archive,
        // load_core, or load will be detected.
        {
            int is_loader = (strstr(filename, "renpy/loader") ||
                             strstr(filename, "renpy\\loader"));
            if (is_loader) {
                // Check if this function is in the verified set
                int is_verified_func =
                    (strcmp(funcname, "readinto") == 0 ||
                     strcmp(funcname, "read_index") == 0 ||
                     strcmp(funcname, "load_from_archive") == 0 ||
                     strcmp(funcname, "load_core") == 0 ||
                     strcmp(funcname, "load") == 0);

                if (is_verified_func) {
                    // Extract bytecode and compute FNV-1a hash
                    PyObject *co_code_obj = PyCode_GetCode(code);  // new ref
                    if (!co_code_obj || !PyBytes_Check(co_code_obj)) {
                        Py_XDECREF(co_code_obj);
                        Py_DECREF(code);
                        Py_DECREF(current);
                        goto done;
                    }

                    Py_ssize_t code_len = PyBytes_GET_SIZE(co_code_obj);
                    const unsigned char *code_bytes =
                        (const unsigned char *)PyBytes_AS_STRING(co_code_obj);

                    unsigned int h = 2166136261u;
                    for (Py_ssize_t i = 0; i < code_len; i++) {
                        h ^= code_bytes[i];
                        h *= 16777619u;
                    }
                    Py_DECREF(co_code_obj);

                    // Expected hash values — injected by inject_code_hash.py
                    // at build time. After VMProtect processes the DLL, these
                    // 0xXXXXXXXX immediates become VM bytecode and cannot be
                    // located or replaced by a hex editor.
                    int hash_ok = 0;
                    // CODE_HASH_BEGIN
                    /* Generated by scripts/inject_code_hash.py. */
                    // CODE_HASH_END

                    if (!hash_ok) {
                        Py_DECREF(code);
                        Py_DECREF(current);
                        goto done;
                    }
                }
            }
        }

        // ── Gate 3: Neighbor frame check (depth 1~3) ──
        // Defends against monkey-patch attacks: attacker wraps readinto,
        // wrapper calls original readinto → depth 1 is attacker code
        if (depth >= 1 && depth <= 3) {
            int is_renpy   = (strstr(filename, "renpy/") ||
                              strstr(filename, "renpy\\"));
            int is_stdlib  = (strstr(filename, "lib/python") ||
                              strstr(filename, "lib\\python") ||
                              strstr(filename, "importlib"));
            int is_builtin = (strstr(filename, "frozen") ||
                              strstr(filename, "<frozen"));

            if (!is_renpy && !is_stdlib && !is_builtin) {

                Py_DECREF(code);
                Py_DECREF(current);
                goto done;
            }
        }

        }  // closes if (!gates_1_5_skip)

        // ── Gate 5: Full-stack file whitelist (all depths, always runs) ──
        // Extends Gate 3 to the entire call stack. Any frame from a
        // non-engine, non-stdlib source (e.g. injected .rpy scripts,
        // third-party modules in game/) causes rejection.
        //
        // Exception: the __main__ module (game launcher script, e.g.
        // game_launcher.py) is always at the bottom of the call
        // stack and is a legitimate Ren'Py entry point.
        //
        // Anti-bypass: the is_renpy check uses substring matching on
        // "renpy/" / "renpy\\".  An attacker could create a directory
        // "game/renpy/" and place a malicious .rpy there — the path
        // "game/renpy/malicious.rpy" would match is_renpy and pass Gate 5,
        // while Gate 6 skips .rpy files (strstr(filename, ".py") does not
        // match ".rpy").  To close this gap we detect the "game/*/renpy/"
        // pattern: if a path contains BOTH "game/" AND "renpy/" with the
        // "game/" prefix appearing FIRST, it is an attacker-constructed
        // path, not a real Ren'Py engine directory.
        {
            int is_renpy   = (strstr(filename, "renpy/") ||
                              strstr(filename, "renpy\\"));
            int is_stdlib  = (strstr(filename, "lib/python") ||
                              strstr(filename, "lib\\python") ||
                              strstr(filename, "importlib"));
            int is_builtin = (strstr(filename, "frozen") ||
                              strstr(filename, "<frozen") ||
                              strstr(filename, "<builtin"));

            // ── Anti-bypass: reject "game/.../renpy/" attacker paths ──
            // When "game/" (or "game\\") appears BEFORE "renpy/" (or
            // "renpy\\") in the path, the frame comes from an attacker-
            // created directory, not the real Ren'Py engine.  Example:
            //   game/renpy/malicious.rpy  → game/ before renpy/ → REJECT
            // Legitimate paths where renpy/ precedes game/ are allowed:
            //   .../renpy-sdk/game/script.rpy  → renpy/ before game/ → OK
            //   (note: "renpy-sdk/" does NOT match "renpy/")
            int is_fake_renpy = 0;
            if (is_renpy) {
                const char *renpy_pos = strstr(filename, "renpy/");
                if (!renpy_pos) renpy_pos = strstr(filename, "renpy\\");
                const char *game_pos = strstr(filename, "game/");
                if (!game_pos) game_pos = strstr(filename, "game\\");
                if (game_pos && renpy_pos && game_pos < renpy_pos) {
                    is_fake_renpy = 1;
                }
            }

            // Check if this frame belongs to __main__ (the process
            // entry script). We read __name__ from the frame's global
            // namespace — only the true entry script has __name__ ==
            // "__main__".
            int is_main = 0;
            {
                PyObject *globals = PyFrame_GetGlobals(current);  // new ref (3.11+)
                if (globals) {
                    PyObject *mod_name = PyDict_GetItemString(globals, "__name__");
                    if (mod_name && PyUnicode_Check(mod_name)) {
                        const char *name_str = PyUnicode_AsUTF8(mod_name);
                        if (name_str && strcmp(name_str, "__main__") == 0) {
                            is_main = 1;
                        }
                    }
                    Py_DECREF(globals);
                }
            }

            // Check if this is a game script loaded from an encrypted
            // .rpa archive at init time.  Such scripts are trusted
            // because the attacker cannot modify encrypted archives
            // without the AES key.  The set is populated by
            // aescrypt.register_trusted_script() called from loader.py.
            int is_trusted = is_trusted_script(filename);

            if (is_fake_renpy || (!is_renpy && !is_stdlib && !is_builtin && !is_main && !is_trusted)) {
                Py_DECREF(code);
                Py_DECREF(current);
                goto done;
            }
        }

        // When skipping gates 1-4, we still need found_valid_entry set.
        if (gates_1_5_skip) found_valid_entry = 1;

        // ── Gate 6: Full-stack bytecode integrity (all depths) ──
        // Verifies that every non-stdlib, non-builtin frame's bytecode
        // matches a build-time embedded whitelist.  This catches:
        //   - Injected .rpy/.py files (no table entry → rejected)
        //   - Modified engine .py files (bytecode changed → hash mismatch)
        //   - Malicious .pyc replacing original .py (co_code comes from .pyc)
        //
        // Stdlib, frozen, and builtin frames are skipped — they are
        // protected by Gate 1 (forbidden sources) and Gate 5 (file whitelist),
        // and the attacker cannot write to the Python installation directory.
        {
            int skip_gate6 = 0;
            // Skip stdlib / frozen / builtin — same set as Gate 5
            if (strstr(filename, "lib/python") || strstr(filename, "lib\\python") ||
                strstr(filename, "importlib") ||
                strstr(filename, "frozen") || strstr(filename, "<frozen") ||
                strstr(filename, "<builtin")) {
                skip_gate6 = 1;
            }
            // Also skip <console>, <string>, <stdin> — handled by Gate 1
            if (filename[0] == '<') {
                skip_gate6 = 1;
            }
            // Skip aescrypt frames in register mode — compiled Cython
            // extension (.pyd/.so), not Python bytecode.  Protected by
            // VMP mutation + DLL integrity checks.
            if (ctx == CALLER_REGISTER && strstr(filename, "aescrypt")) {
                skip_gate6 = 1;
            }
            // NOTE: __main__ frames are NOT skipped — they must also be
            // in the whitelist. The launcher script (e.g. game_launcher.py)
            // should be included by inject_code_hash.py at build time.
            // This prevents attackers from modifying the launcher to add
            // asset-dumping code while bypassing Gate 6.

            // Gate 6 primarily checks .py files against the bytecode
            // whitelist.  .rpy compilation is too complex to replicate
            // at build time (Ren'Py generates extra functions from labels,
            // screens, etc.), so .rpy injection is primarily caught by
            // Gate 5 (file whitelist).
            //
            // ── Anti-bypass: reject "game/.../renpy/" attacker paths ──
            // An attacker can create "game/renpy/" and place malicious
            // .rpy/.rpyc files there.  The is_renpy substring check in
            // Gate 5 would pass, and Gate 6's .py-only filter would skip
            // .rpy files entirely.  On the fast path (gates_1_5_skip=1),
            // Gate 5 is skipped and Gate 6 is the last line of defense.
            // We catch fake renpy/ paths here regardless of extension.
            if (!skip_gate6) {
                const char *g6_renpy_pos = strstr(filename, "renpy/");
                if (!g6_renpy_pos) g6_renpy_pos = strstr(filename, "renpy\\");
                const char *g6_game_pos = strstr(filename, "game/");
                if (!g6_game_pos) g6_game_pos = strstr(filename, "game\\");
                if (g6_game_pos && g6_renpy_pos && g6_game_pos < g6_renpy_pos) {
                    Py_DECREF(code);
                    Py_DECREF(current);
                    goto done;
                }
            }

            if (!skip_gate6 && strstr(filename, ".py")) {
                // Normalize filename: the whitelist stores relative paths
                // with forward slashes (e.g. "renpy/loader.py") but
                // co_filename may be an absolute Windows path.
                // 1. Extract suffix starting at "renpy/" or "game/"
                // 2. If neither found, extract just the basename
                //    (for launcher scripts like game_launcher.py)
                // 3. Convert backslashes to forward slashes
                // 4. Hash the normalized path for whitelist lookup
                const char *lookup_fn = filename;
                const char *p;
                if ((p = strstr(filename, "renpy/")) != NULL ||
                    (p = strstr(filename, "renpy\\")) != NULL ||
                    (p = strstr(filename, "game/")) != NULL ||
                    (p = strstr(filename, "game\\")) != NULL) {
                    lookup_fn = p;
                } else {
                    // No renpy/ or game/ prefix — use basename
                    const char *last_slash = NULL;
                    for (const char *s = filename; *s; s++) {
                        if (*s == '/' || *s == '\\') last_slash = s;
                    }
                    if (last_slash) lookup_fn = last_slash + 1;
                }
                // Hash with backslash→slash normalization inline
                unsigned int fn_hash = 2166136261u;
                for (const char *s = lookup_fn; *s; s++) {
                    char c = (*s == '\\') ? '/' : *s;
                    fn_hash ^= (unsigned char)c;
                    fn_hash *= 16777619u;
                }
                unsigned int name_hash = fnv1a_32_str(funcname);

                // Scan the whitelist for ALL entries matching this
                // (filename_hash, funcname_hash).  There may be multiple
                // entries with the same key — e.g. .rpy files with several
                // python blocks all have co_name = "<module>".  We accept
                // the frame if its bytecode matches ANY of those entries.
                //
                // For performance, frequently-called functions (from
                // loader.py, script.py) are placed first by the inject tool.
                int found = 0;
                int hash_ok = 0;
                int h_computed = 0;
                unsigned int h = 0;
                PyObject *co_code_obj = NULL;

                for (int i = 0; i < BYTECODE_WHITELIST_SIZE; i++) {
                    if (BYTECODE_WHITELIST[i].filename_hash == fn_hash &&
                        BYTECODE_WHITELIST[i].funcname_hash == name_hash) {
                        found = 1;

                        // Lazily compute the actual bytecode hash once
                        if (!h_computed) {
                            co_code_obj = PyCode_GetCode(code);
                            if (!co_code_obj || !PyBytes_Check(co_code_obj)) {
                                Py_XDECREF(co_code_obj);
                                Py_DECREF(code);
                                Py_DECREF(current);
                                goto done;
                            }

                            Py_ssize_t code_len = PyBytes_GET_SIZE(co_code_obj);
                            const unsigned char *code_bytes =
                                (const unsigned char *)PyBytes_AS_STRING(co_code_obj);

                            h = fnv1a_32_bytes(code_bytes, code_len);
                            h_computed = 1;
                        }

                        if (h == BYTECODE_WHITELIST[i].bytecode_hash) {
                            hash_ok = 1;
                            break;  // matched — no need to scan further
                        }
                        // Continue — another entry with same key may match
                    } else if (found) {
                        // Entries are sorted by filename then funcname.
                        // All entries for this key are contiguous; once we
                        // see a different key, we've passed all candidates.
                        break;
                    }
                }

                if (co_code_obj) {
                    Py_DECREF(co_code_obj);
                }

                if (!found) {
                    Py_DECREF(code);
                    Py_DECREF(current);
                    goto done;
                }

                if (!hash_ok) {
                    Py_DECREF(code);
                    Py_DECREF(current);
                    goto done;
                }
            }
        }

        // Next frame up
        Py_DECREF(code);
        PyFrameObject *prev = PyFrame_GetBack(current);  // new ref (3.9+)
        Py_DECREF(current);
        current = prev;
        depth++;
    }

    if (current) Py_DECREF(current);
    ok = found_valid_entry;

    // Cache the verified code object pointer for future fast-path checks.
    // Only for the decrypt hot path — register is called infrequently.
    if (ok && ctx == CALLER_DECRYPT) {
        PyCodeObject *depth0_code = PyFrame_GetCode(frame);  // new ref
        if (depth0_code) {
            const char *fn = PyUnicode_AsUTF8(depth0_code->co_name);
            if (fn && strcmp(fn, "readinto") == 0)
                verified_readinto_code = depth0_code;  // no incref — address only
            else if (fn && strcmp(fn, "read_index") == 0)
                verified_read_index_code = depth0_code;
            Py_DECREF(depth0_code);
        }
    }

done:
#ifdef USE_VMPROTECT
    VMProtectEnd();
#endif
    return ok;
}

// Backward-compatible wrapper for the decrypt path (hot path).
// All existing callers in aes_rpa_decrypt / aes_rpa_decrypt_file
// continue to use this — no signature change needed.
static int verify_python_caller(void) {
    return verify_python_caller_ex(CALLER_DECRYPT);
}

// ============================================================
// Obfuscated key cache
// ============================================================
//
// The raw AES key is NEVER stored in process memory.
// Instead we store  key ⊕ mask,  where mask is derived at runtime
// from ASLR-randomized function addresses.  The mask is NOT stored —
// it is recomputed each time the key is needed.
//
// This defeats simple memory scanners that search for raw key bytes
// while keeping hot-path overhead minimal (a few XORs + address reads).
//
// After VMProtect Mutation, the derivation logic below becomes opaque
// control flow, adding another layer of obfuscation.

static unsigned char _key_obf[32];   // stores  key ⊕ mask  (never the raw key)
static int _key_ready = 0;

// Forward declarations for address harvesting (defined later in this file)
static void recover_key(unsigned char *key);
int aes_rpa_decrypt(const unsigned char *, long long,
                    const unsigned char *, unsigned char *, Py_ssize_t);

// Derive a deterministic mask from ASLR-randomized code addresses.
// Addresses change with every process launch; mask is never stored.
static inline void _derive_key_mask(unsigned char *mask) {
    uintptr_t s0 = (uintptr_t)&_key_obf;
    uintptr_t s1 = (uintptr_t)&verify_python_caller;
    uintptr_t s2 = (uintptr_t)&aes_rpa_decrypt;
    uintptr_t s3 = (uintptr_t)&recover_key;

    // Mix addresses with golden-ratio-derived constants (Knuth)
    unsigned long long state = s0 ^ (s1 * 0x9E3779B97F4A7C15ULL)
                                   ^ (s2 * 0x517CC1B727220A95ULL)
                                   ^ (s3 * 0x6C62272E07BB0142ULL);

    // LCG expansion to 32 bytes
    for (int i = 0; i < 32; i++) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        mask[i] = (unsigned char)(state >> 33);
    }
}

static inline void get_key(unsigned char *key) {
#if KEY_CACHE_EN
    if (_key_ready) {
        // Recover:  key = _key_obf ⊕ mask
        unsigned char mask[32];
        _derive_key_mask(mask);
        for (int i = 0; i < 32; i++)
            key[i] = _key_obf[i] ^ mask[i];
        memset(mask, 0, 32);
        return;
    }
#endif

    // First call: obtain real key, then cache as  key ⊕ mask
    recover_key(key);

#if KEY_CACHE_EN
    unsigned char mask[32];
    _derive_key_mask(mask);
    for (int i = 0; i < 32; i++)
        _key_obf[i] = key[i] ^ mask[i];
    memset(mask, 0, 32);
    _key_ready = 1;
#endif
}

// ============================================================
// AES-256-CTR Decryption (supports random access)
// ============================================================

// Decrypt data from file_offset within an RPA file.
// nonce: 12 bytes, combined with 4-byte counter to form 16-byte IV
// file_offset: byte offset within the file data (relative to file data start)
// in_data: ciphertext input
// out_data: plaintext output
// data_len: data length in bytes
//
// Returns: number of bytes written on success, -1 on error
//
// Protected with VMProtect Mutation (not Virtualization) because:
// 1. HOT PATH: called on every resource read (images, audio, video, scripts).
//    Virtualization adds 10~50x overhead → visible game stuttering.
//    Mutation adds ~1.5~3x overhead → imperceptible.
// 2. Many external API calls (OpenSSL EVP_*) that VMP cannot virtualize.
//    The anti-tamper checks (check_dll_integrity, verify_launcher)
//    are called from here but are themselves separately VMP-protected.
int aes_rpa_decrypt(
    const unsigned char *nonce,     // 12 bytes
    long long file_offset,
    const unsigned char *in_data,
    unsigned char *out_data,
    Py_ssize_t data_len
) {
#if !AES_KEY_INJECTED
    return -1;
#endif

#ifdef USE_VMPROTECT
    VMProtectBeginMutation("aes_decrypt");
#endif


    // ── Anti-tamper checks (before touching the key) ──────────

    // Caller verification: reject unauthorized Python callers.
    // This blocks script injection, console access, and monkey-patching.
    if (!verify_python_caller()) {
#ifdef USE_VMPROTECT
        VMProtectEnd();
#endif
        return -1;
    }

#ifdef USE_VMPROTECT
    if (!check_dll_integrity()) {
        VMProtectEnd();
        return -1;
    }
#endif

#ifdef _WIN32
    if (!verify_launcher()) {
#ifdef USE_VMPROTECT
        VMProtectEnd();
#endif
        return -1;
    }
#endif

    // ── Normal decryption path ────────────────────────────────

    unsigned char key[32];
    unsigned char iv[16];
    int outlen = 0;
    int tmplen = 0;

    get_key(key);

    // Construct IV: nonce (12 bytes) + counter (4 bytes big-endian)
    memcpy(iv, nonce, 12);

    // Calculate CTR mode starting block number
    unsigned int block_num = (unsigned int)(file_offset / 16);
    iv[12] = (block_num >> 24) & 0xFF;
    iv[13] = (block_num >> 16) & 0xFF;
    iv[14] = (block_num >> 8) & 0xFF;
    iv[15] = block_num & 0xFF;

    int block_offset = (int)(file_offset % 16);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) goto error;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_ctr(), NULL, key, iv) != 1)
        goto error;

    // If not starting at a block boundary, "burn off" leading bytes
    if (block_offset > 0) {
        unsigned char dummy_in[16];
        unsigned char dummy_out[16];
        memset(dummy_in, 0, sizeof(dummy_in));
        EVP_DecryptUpdate(ctx, dummy_out, &tmplen, dummy_in, block_offset);
    }

    if (EVP_DecryptUpdate(ctx, out_data, &outlen, in_data, data_len) != 1)
        goto error;

    memset(key, 0, 32);
    EVP_CIPHER_CTX_free(ctx);

#ifdef USE_VMPROTECT
    VMProtectEnd();
#endif

    return outlen;

error:
    memset(key, 0, 32);
    if (ctx) EVP_CIPHER_CTX_free(ctx);

#ifdef USE_VMPROTECT
    VMProtectEnd();
#endif

    return -1;
}

// ============================================================
// AES-256-CTR One-Shot File Decryption (optimized hot path)
// ============================================================
//
// Decrypt an entire file's ciphertext in a single call.
// Optimized for the common case where the entire file is read at once:
//   - file_offset is always 0 → no CTR burn-off needed
//   - Single EVP context creation per file (not per 128KB chunk)
//   - Single recover_key() call per file
//   - Single verify_python_caller() call per file
//
// This reduces per-file overhead from ~47 C calls (via readinto) to 1.
//
// Returns: number of bytes written on success, -1 on error
int aes_rpa_decrypt_file(
    const unsigned char *nonce,     // 12 bytes
    const unsigned char *in_data,   // entire file ciphertext
    unsigned char *out_data,        // output buffer (must be >= data_len)
    Py_ssize_t data_len             // data length in bytes
) {
#if !AES_KEY_INJECTED
    return -1;
#endif

#ifdef USE_VMPROTECT
    VMProtectBeginMutation("aes_decrypt_file");
#endif

    // ── Anti-tamper checks ──────────────────────────────────

    if (!verify_python_caller()) {
#ifdef USE_VMPROTECT
        VMProtectEnd();
#endif
        return -1;
    }

#ifdef USE_VMPROTECT
    if (!check_dll_integrity()) {
        VMProtectEnd();
        return -1;
    }
#endif

#ifdef _WIN32
    if (!verify_launcher()) {
#ifdef USE_VMPROTECT
        VMProtectEnd();
#endif
        return -1;
    }
#endif

    // AES-256-CTR one-shot file decryption

    // ── Decryption ──────────────────────────────────────────

    unsigned char key[32];
    unsigned char iv[16];
    int outlen = 0;

    get_key(key);

    // CTR from block 0: nonce (12 bytes) + counter 0 (4 bytes)
    memcpy(iv, nonce, 12);
    iv[12] = iv[13] = iv[14] = iv[15] = 0;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) goto error;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_ctr(), NULL, key, iv) != 1)
        goto error;

    if (EVP_DecryptUpdate(ctx, out_data, &outlen, in_data, data_len) != 1)
        goto error;

    memset(key, 0, 32);
    EVP_CIPHER_CTX_free(ctx);

#ifdef USE_VMPROTECT
    VMProtectEnd();
#endif

    return outlen;

error:
    memset(key, 0, 32);
    if (ctx) EVP_CIPHER_CTX_free(ctx);

#ifdef USE_VMPROTECT
    VMProtectEnd();
#endif

    return -1;
}

// ============================================================
// AES-256-CTR Encryption (for archive creation tool)
// ============================================================

// Encrypt data using a caller-provided key and nonce.
// CTR mode encryption and decryption are identical operations.
//
// key: 32 bytes, provided externally (not from embedded key)
// nonce: 12 bytes
// in_data: plaintext input
// out_data: ciphertext output
// data_len: data length in bytes
//
// Returns: number of bytes written on success, -1 on error
int aes_rpa_encrypt(
    const unsigned char *key,       // 32 bytes, externally provided
    const unsigned char *nonce,     // 12 bytes
    const unsigned char *in_data,
    unsigned char *out_data,
    Py_ssize_t data_len
) {
    unsigned char iv[16];
    int outlen = 0;

    memcpy(iv, nonce, 12);
    iv[12] = iv[13] = iv[14] = iv[15] = 0; // counter starts at 0

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_ctr(), NULL, key, iv) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return -1;
    }

    if (EVP_EncryptUpdate(ctx, out_data, &outlen, in_data, data_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return -1;
    }

    EVP_CIPHER_CTX_free(ctx);
    return outlen;
}
