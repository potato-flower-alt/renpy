# Copyright 2004-2026 Tom Rothamel <pytom@bishoujo.us>
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

# Ren'Py archiver. This builds a Ren'Py archive file, and the
# associated index file. These files are really easy to
# reverse-engineer, but are probably better than nothing.

init python in archiver:

    import sys
    import random
    import glob
    import zlib
    import os
    import hmac
    import hashlib

    from pickle import dumps, HIGHEST_PROTOCOL


    class Archive(object):
        """
        Adds files from disk to a rpa archive.
        """

        def __init__(self, filename):

            # The archive file.
            self.f = open(filename, "wb")

            # The index to the file.
            self.index = _dict()

            # A fixed key minimizes difference between archive versions.
            self.key = 0x42424242

            padding = b"RPA-3.0 XXXXXXXXXXXXXXXX XXXXXXXX\n"
            self.f.write(padding)

        def add(self, name, path):
            """
            Adds a file to the archive.
            """

            self.index[name] = _list()

            with open(path, "rb") as df:
                data = df.read()
                dlen = len(data)

            # Pad.
            padding = b"Made with Ren'Py."
            self.f.write(padding)

            offset = self.f.tell()

            self.f.write(data)

            self.index[name].append((offset ^ self.key, dlen ^ self.key, b""))

        def close(self):

            indexoff = self.f.tell()

            self.f.write(zlib.compress(dumps(self.index, HIGHEST_PROTOCOL)))

            self.f.seek(0)
            self.f.write(b"RPA-3.0 %016x %08x\n" % (indexoff, self.key))

            self.f.close()

    class EncryptedArchive(object):
        """
        Creates an AES-256-CTR encrypted RPA archive (RPAE-2.0 format).

        The XOR key and index nonce are derived deterministically from the
        AES key and archive name via HMAC-SHA256, ensuring that the same
        key and inputs produce byte-identical archives on every build.
        """

        def __init__(self, filename, key):
            """
            key: 32-byte AES-256 key (bytes)
            """
            # The archive file.
            self.f = open(filename, "wb")

            # The index to the file.
            self.index = _dict()

            self.key = key

            self.archive_name = os.path.basename(filename)

            xor_key_bytes = hmac.digest(
                self.key,
                b"rpa_xor_key\0" + self.archive_name.encode("utf-8"),
                hashlib.sha256,
            )[:4]
            self.xor_key = int.from_bytes(xor_key_bytes, 'big')

            self.f.write(b"RPAE-2.0 XXXXXXXXXXXXXXXX XXXXXXXX\n")
            self.f.write(b'\x00' * 12)

        def add(self, name, path):
            """
            Encrypts and adds a file to the archive.
            """
            import renpy.aescrypt as aescrypt

            self.index[name] = _list()

            with open(path, "rb") as df:
                data = df.read()
                dlen = len(data)

            nonce = hmac.digest(
                self.key,
                name.encode("utf-8") + b"\0" + data,
                hashlib.sha256,
            )[:12]

            encrypted_data = aescrypt.encrypt_block(self.key, nonce, data)

            # Write padding
            self.f.write(b"Made with Ren'Py.")

            offset = self.f.tell()
            self.f.write(encrypted_data)

            self.index[name].append((
                offset ^ self.xor_key,
                dlen ^ self.xor_key,
                nonce
            ))

        def close(self):
            import renpy.aescrypt as aescrypt

            indexoff = self.f.tell()

            # Serialize and compress the index
            index_data = zlib.compress(dumps(self.index, HIGHEST_PROTOCOL))

            index_nonce = hmac.digest(
                self.key,
                b"rpa_index_nonce\0"
                + self.archive_name.encode("utf-8")
                + b"\0"
                + hashlib.sha256(index_data).digest(),
                hashlib.sha256,
            )[:12]

            encrypted_index = aescrypt.encrypt_block(
                self.key, index_nonce, index_data
            )

            self.f.write(encrypted_index)

            self.f.seek(40)
            self.f.write(index_nonce)

            self.f.seek(0)
            self.f.write(
                b"RPAE-2.0 %016x %08x\n" % (indexoff, self.xor_key)
            )

            self.f.close()
