#!/usr/bin/env python3
"""Writes the ZIP archives used by the extraction tests into a folder."""

import os
import struct
import sys
import warnings
import zipfile

BINARY = bytes(range(256)) * 40
README = b"hello from a zip\n"


def write(path, entries, compression=zipfile.ZIP_DEFLATED, zip64=False):
    with zipfile.ZipFile(path, "w", compression=compression) as archive:
        for name, data, *options in entries:
            if name.endswith("/"):
                archive.writestr(zipfile.ZipInfo(name), b"")
                continue
            info = zipfile.ZipInfo(name)
            info.compress_type = options[0] if options else compression
            with archive.open(info, "w", force_zip64=zip64) as handle:
                handle.write(data)


def main(folder):
    os.makedirs(folder, exist_ok=True)
    join = lambda name: os.path.join(folder, name)

    write(join("basic.zip"), [
        ("readme.txt", README),
        ("3ds/App/", b""),
        ("3ds/App/App.3dsx", BINARY, zipfile.ZIP_STORED),
        ("empty.txt", b""),
    ])

    write(join("evil.zip"), [
        ("../escape.txt", b"x"),
        ("/abs.txt", b"x"),
        ("C:/drive.txt", b"x"),
        ("ok/../../up.txt", b"x"),
        ("dir\\win.txt", b"windows separators\n"),
        ("good.txt", b"good\n"),
    ])

    write(join("zip64.zip"), [("big.bin", BINARY)], zip64=True)

    write(join("mixed.zip"), [
        ("bz.txt", b"packed with bzip2\n", zipfile.ZIP_BZIP2),
        ("plain.txt", b"plain\n"),
    ])

    write(join("utf8.zip"), [("na\u00efve.txt", b"utf8\n")])

    write(join("rooted.zip"), [
        ("Game/", b""),
        ("Game/a.txt", b"a\n"),
        ("Game/sub/b.txt", b"b\n"),
    ])

    # The same name twice, the second shorter: the last one wins, whole.
    with zipfile.ZipFile(join("dupes.zip"), "w") as archive, \
            warnings.catch_warnings():
        warnings.simplefilter("ignore")
        archive.writestr("same.txt", b"the first, longer copy\n")
        archive.writestr("same.txt", b"second\n")

    # The same for files too large for one write, which go through a
    # temporary name.
    with zipfile.ZipFile(join("dupes-big.zip"), "w") as archive, \
            warnings.catch_warnings():
        warnings.simplefilter("ignore")
        archive.writestr("big.bin", b"1" * (300 * 1024))
        archive.writestr("big.bin", b"2" * (300 * 1024))

    # Mark the first entry as PPMd (98), which neither reader extracts.
    write(join("odd.zip"), [("odd.bin", b"odd\n"), ("plain.txt", b"plain\n")],
          compression=zipfile.ZIP_STORED)
    with open(join("odd.zip"), "r+b") as handle:
        data = bytearray(handle.read())
        data[8:10] = struct.pack("<H", 98)
        central = data.index(b"PK\x01\x02")
        data[central + 10:central + 12] = struct.pack("<H", 98)
        handle.seek(0)
        handle.write(data)

    # One name in the central directory, another in the local header that a
    # sender's archive would also allow, and a bzip2 entry so libarchive
    # (which reads the local one) extracts it.
    write(join("renamed.zip"), [
        ("3ds/a/abcd.3dsx", b"planted\n", zipfile.ZIP_STORED),
        ("3ds/a/bz.txt", b"packed with bzip2\n", zipfile.ZIP_BZIP2),
    ])
    with open(join("renamed.zip"), "r+b") as handle:
        data = bytearray(handle.read())
        data[30:30 + 15] = b"3ds/a/zzzz.3dsx"
        handle.seek(0)
        handle.write(data)

    # Flip one byte of stored data so the CRC check must fail.
    write(join("corrupt.zip"), [("data.bin", b"A" * 1000)],
          compression=zipfile.ZIP_STORED)
    with open(join("corrupt.zip"), "r+b") as handle:
        header = handle.read(30)
        name_length, extra_length = struct.unpack("<HH", header[26:30])
        handle.seek(30 + name_length + extra_length + 10)
        handle.write(b"B")

    with open(join("basic.zip"), "rb") as handle:
        data = handle.read()
    with open(join("truncated.zip"), "wb") as handle:
        handle.write(data[: len(data) // 2])


if __name__ == "__main__":
    main(sys.argv[1])
