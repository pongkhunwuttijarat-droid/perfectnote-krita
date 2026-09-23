#!/usr/bin/env python3
"""Build the multipage-render fixture used by PdfStripCursorTest.

Deterministic and dependency-free: the PDF is assembled here, the way
ex-make-fixtures.py does it. Three pages of mixed size, each a solid colour with its number
printed on it, so a rendered strip shows at a glance which slot holds which page and the test can
assert it by pixel.

Run from anywhere:

    python3 krita/plugins/extensions/pdfio/tests/data/mr-make-fixtures.py

It writes mr-strip-3page.pdf next to itself.
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "mr-strip-3page.pdf")
NL = b"\n"

# (width_pt, height_pt, r, g, b, label) -- A4 portrait, A5 landscape, a small square.
PAGES = [
    (595, 842, 0.85, 0.20, 0.20, b"PAGE 1"),
    (595, 420, 0.20, 0.65, 0.30, b"PAGE 2"),
    (300, 300, 0.20, 0.35, 0.85, b"PAGE 3"),
]


def content(width, height, r, g, b, label):
    body = (
        b"%.3f %.3f %.3f rg\n" % (r, g, b)
        + b"0 0 %d %d re f\n" % (width, height)
        + b"0 0 0 rg\n"
        + b"BT /F1 36 Tf 24 24 Td (%s) Tj ET\n" % label
        + b"1 1 1 rg\n"
        + b"BT /F1 36 Tf 26 26 Td (%s) Tj ET\n" % label
    )
    return body


def build():
    objects = []
    # 1 catalog, 2 pages, then per page a page object and a content object, then the font.
    font_obj = 3 + 2 * len(PAGES)
    kids = []
    for i in range(len(PAGES)):
        kids.append(b"%d 0 R" % (3 + 2 * i))

    objects.append(b"<< /Type /Catalog /Pages 2 0 R >>")
    objects.append(b"<< /Type /Pages /Kids [" + b" ".join(kids) + b"] /Count %d >>" % len(PAGES))

    for i, (w, h, r, g, b, label) in enumerate(PAGES):
        stream = content(w, h, r, g, b, label)
        objects.append(
            b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] "
            b"/Resources << /Font << /F1 %d 0 R >> >> /Contents %d 0 R >>"
            % (w, h, font_obj, 4 + 2 * i)
        )
        objects.append(
            b"<< /Length " + str(len(stream)).encode() + b" >>" + NL
            + b"stream" + NL + stream + b"endstream"
        )

    objects.append(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")

    out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = []
    for number, body in enumerate(objects, start=1):
        offsets.append(len(out))
        out += b"%d 0 obj\n" % number + body + b"\nendobj\n"

    xref_at = len(out)
    out += b"xref\n0 %d\n" % (len(objects) + 1)
    out += b"0000000000 65535 f \n"
    for offset in offsets:
        out += b"%010d 00000 n \n" % offset
    out += (
        b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n"
        % (len(objects) + 1, xref_at)
    )
    return bytes(out)


with open(OUT, "wb") as handle:
    handle.write(build())
print("wrote", OUT, os.path.getsize(OUT), "bytes")
