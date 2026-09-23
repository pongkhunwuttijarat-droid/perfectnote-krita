#!/usr/bin/env python3
"""Builds the one fixture KZip cannot write itself: an archive with an absolute entry name.

KZip strips a leading "/" when it writes, so an entry stored as "/tmp/x" would come back out as
"tmp/x" and the case could never be produced. Python's zipfile does not strip it, so the file that
lands in the tree really does carry the absolute name -- verified with `unzip -l`.

The notebook around the entry is a valid one-page project, so the only unusual thing about the file
is the entry name. Everything is derived from a fixed seed, so rebuilding it produces the same bytes.

    python3 krita/plugins/extensions/pdfio/tests/data/nb-make-fixtures.py
"""

import hashlib
import json
import pathlib
import zipfile


def filler(seed: str, size: int) -> bytes:
    """Deterministic, incompressible bytes, the same function the Qt test uses."""
    state = 2166136261
    for ch in seed.encode():
        state = ((state ^ ch) * 16777619) & 0xFFFFFFFF
    data = bytearray()
    while len(data) < size:
        state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
        data.append((state >> 24) & 0xFF)
    return bytes(data)


def build() -> pathlib.Path:
    source = filler("nb-source", 2048)
    manifest = {
        "schema": 1,
        "source": {
            "file": "source.pdf",
            "sha256": hashlib.sha256(source).hexdigest(),
            "bytes": len(source),
        },
        "pages": [
            {
                "index": 0,
                "sizePt": [595, 842],
                "rotation": 0,
                "kra": "pages/p0001.kra",
                "thumb": "thumbs/p0001.png",
                "generation": 0,
            }
        ],
    }

    entries = [
        ("manifest.json", json.dumps(manifest, indent=4).encode()),
        ("source.pdf", source),
        ("pages/p0001.kra", filler("nb-ink", 1024)),
        ("thumbs/p0001.png", filler("nb-thumb", 256)),
        ("/tmp/nb-absolute-escape.txt", b"an entry whose stored name is absolute\n"),
    ]

    out = pathlib.Path(__file__).resolve().with_name("nb-absolute-entry.pnb")
    out.unlink(missing_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, data in entries:
            info = zipfile.ZipInfo(name, (2001, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(info, data)
    return out


if __name__ == "__main__":
    path = build()
    print(path, path.stat().st_size, "bytes")
