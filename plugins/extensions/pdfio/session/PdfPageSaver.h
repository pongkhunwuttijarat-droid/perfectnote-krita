/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFPAGESAVER_H
#define PDFPAGESAVER_H

#include <QString>

class KisDocument;
class QIODevice;

/**
 * Writes one page of a project.
 *
 * The whole size budget rests on this: Krita normally writes a mergedimage.png with every
 * KRA, and for a page that is a full A4 render that one entry dwarfs the notes themselves.
 * The page artwork is derivable from the bundled source PDF, so persisting it is pure
 * duplication, and KraConverter already exposes the switch that skips it (it is the same one
 * autosave uses).
 *
 * Only the ink is written, so a reader has to put the page back underneath it; the session
 * keeps the geometry to do that in manifest.json.
 */
class PdfPageSaver
{
public:
    static bool saveInkOnly(KisDocument *document,
                            QIODevice *io,
                            const QString &fileName,
                            QString *why = nullptr);
};

#endif // PDFPAGESAVER_H
