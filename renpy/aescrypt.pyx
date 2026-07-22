# Copyright 2004-2026 Tom Rothamel <pytom@bishoujo.us>
# Copyright 2026 The Ren'Py fork contributors
# SPDX-License-Identifier: MIT
#
# Permission is hereby granted, free of charge, to any person
# obtaining a copy of this software and associated documentation files
# (the "Software"), to deal in the Software without restriction,
# including without limitation the rights to use, copy, modify, merge,
# publish, distribute, sublicense, and/or sell copies of the Software,
# and to permit persons to whom the Software is furnished to do so,
# subject to the following conditions:
#
# The above copyright notice and this permission notice shall be
# included in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
# LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
# WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

# renpy/aescrypt.pyx
# RPA AES-256-CTR Encryption/Decryption Python Interface
#
# This Cython module bridges the C-level AES implementation (aes_rpa.c)
# with Python code in loader.py and archiver.rpy.

from libc.stdlib cimport malloc, free
from libc.string cimport memcpy, memset

cdef extern from "aes_rpa.c":
    int aes_rpa_decrypt(
        const unsigned char *nonce,
        long long file_offset,
        const unsigned char *in_data,
        unsigned char *out_data,
        Py_ssize_t data_len
    )

    int aes_rpa_decrypt_file(
        const unsigned char *nonce,
        const unsigned char *in_data,
        unsigned char *out_data,
        Py_ssize_t data_len
    )

    int aes_rpa_encrypt(
        const unsigned char *key,
        const unsigned char *nonce,
        const unsigned char *in_data,
        unsigned char *out_data,
        Py_ssize_t data_len
    )

    void _c_register_trusted_script(const char *filename)


def decrypt_block(bytes nonce, long long file_offset, bytes data):
    """
    Decrypt a data block using the embedded key.

    nonce: 12-byte random value (unique per file in the archive)
    file_offset: byte offset within the file data
    data: ciphertext data

    Returns: decrypted plaintext as bytes
    """
    if len(nonce) != 12:
        raise ValueError("Nonce must be 12 bytes")

    cdef Py_ssize_t data_len = len(data)
    if data_len == 0:
        return b""

    cdef bytearray out = bytearray(data_len)

    cdef int result = aes_rpa_decrypt(
        <const unsigned char *><char *>nonce,
        file_offset,
        <const unsigned char *><char *>data,
        <unsigned char *><char *>out,
        data_len
    )

    if result < 0:
        raise ValueError("AES decryption failed")

    return bytes(out)


def decrypt_file(bytes nonce, bytes data):
    """
    One-shot decrypt an entire file from offset 0.

    Optimized: no CTR burn-off, single C call per file.
    Use this instead of decrypt_block when decrypting complete files
    from the beginning.

    nonce: 12-byte random value (unique per file in the archive)
    data: entire file's ciphertext

    Returns: decrypted plaintext as bytes
    """
    if len(nonce) != 12:
        raise ValueError("Nonce must be 12 bytes")

    cdef Py_ssize_t data_len = len(data)
    if data_len == 0:
        return b""

    cdef bytearray out = bytearray(data_len)

    cdef int result = aes_rpa_decrypt_file(
        <const unsigned char *><char *>nonce,
        <const unsigned char *><char *>data,
        <unsigned char *><char *>out,
        data_len
    )

    if result < 0:
        raise ValueError("AES decryption failed")

    return bytes(out)


def decrypt_file_inplace(bytes nonce, unsigned char[::1] data):
    """
    Decrypt a buffer in-place (zero-copy).

    AES-CTR is a stream cipher (XOR with keystream), so it is safe to use
    the same memory region for both input and output.  This avoids ALL
    intermediate buffer allocations — the caller's buffer is overwritten
    with plaintext directly.

    nonce: 12-byte nonce
    data:  writable buffer (e.g. bytearray / memoryview of bytearray)
           containing ciphertext; overwritten with plaintext on return.

    Returns: number of bytes decrypted
    """
    if len(nonce) != 12:
        raise ValueError("Nonce must be 12 bytes")

    cdef Py_ssize_t data_len = data.shape[0]
    if data_len == 0:
        return 0

    cdef int result = aes_rpa_decrypt_file(
        <const unsigned char *><char *>nonce,
        &data[0],
        &data[0],       # in-place: output overwrites input
        data_len
    )

    if result < 0:
        raise ValueError("AES decryption failed")

    return result


def encrypt_block(bytes key, bytes nonce, bytes data):
    """
    Encrypt a data block using the specified key (for archive creation).

    key: 32-byte AES-256 key
    nonce: 12-byte random value
    data: plaintext data

    Returns: encrypted ciphertext as bytes
    """
    if len(key) != 32:
        raise ValueError("Key must be 32 bytes")
    if len(nonce) != 12:
        raise ValueError("Nonce must be 12 bytes")

    cdef Py_ssize_t data_len = len(data)
    if data_len == 0:
        return b""

    cdef bytearray out = bytearray(data_len)

    cdef int result = aes_rpa_encrypt(
        <const unsigned char *><char *>key,
        <const unsigned char *><char *>nonce,
        <const unsigned char *><char *>data,
        <unsigned char *><char *>out,
        data_len
    )

    if result < 0:
        raise ValueError("AES encryption failed")

    return bytes(out)


def register_trusted_script(filename):
    """
    Called by the loader when a .rpyc file is successfully loaded from
    an encrypted .rpa archive.  Registers the script's filename so that
    Gate 5 in verify_python_caller() can distinguish legitimate game
    scripts (shipped in encrypted archives) from attacker-injected files
    placed directly on disk.

    filename: the archive index key, e.g. "game/scripts/screens/splashscreen.rpyc"
               Can be str or bytes.
    """
    cdef bytes b
    if isinstance(filename, bytes):
        b = filename
    else:
        b = filename.encode('utf-8')
    _c_register_trusted_script(b)
