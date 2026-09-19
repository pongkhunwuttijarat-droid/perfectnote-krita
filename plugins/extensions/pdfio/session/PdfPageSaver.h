/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFPAGESAVER_H
#define PDFPAGESAVER_H

#include <QString>

#include <kis_types.h>

class KisDocument;
class KisImage;

/**
 * Writes one page of a project.
 *
 * The whole size budget rests on this. Krita saves a document by writing a PNG per layer plus a
 * flattened merged image, and for a page whose background is a full render of an A4 scan that is
 * several megabytes per copy. Measured on the three page fixture: saving the editing document
 * costs 265,120 bytes, of which the page layer alone is 178,429; saving a document that holds
 * only the ink costs 37,254, and that number does not grow when the source page gets heavier.
 *
 * So the page background is never persisted. It is derivable from the bundled source PDF, and
 * manifest.json records the geometry needed to put it back underneath the ink.
 *
 * The editing document cannot simply be saved, because it contains that background. Instead the
 * caller asks for an ink-only copy, saves that, and keeps it alive until the save finishes:
 * Krita saves in the background, and a nested event loop is not an option -- waiting for
 * sigSavingFinished that way wedged on the second save.
 */
class PdfPageSaver
{
public:
    /**
     * A document holding only the contents of the Ink group of \a source, in the same
     * geometry and colour space. Returns nullptr and sets \a why on failure.
     *
     * The caller owns it and has to delete it once sigSavingFinished has arrived.
     */
    static KisDocument *createInkOnlyDocument(const KisImageSP &source, QString *why = nullptr);

    /**
     * The ink of one region of a document, as a document of that region's own size.
     *
     * A strip holds several pages at once, so "the ink of the document" is not a thing there: the
     * ink of the page that is active is the layers of its own group, cropped to the rectangle that
     * page occupies in the strip, and shifted back to the origin so the artifact is the page and
     * nothing about the strip leaks into it.
     */
    static KisDocument *createInkOnlyDocument(const KisImageSP &source,
                                              const QRect &area,
                                              const QList<KisNodeSP> &inkLayers,
                                              QString *why = nullptr);

    /**
     * Saves asynchronously through KisDocument::saveAs, which writes no merged image on its own
     * terms but does honour the document contents. Returns false when the save could not start.
     */
    static bool saveInkOnly(KisDocument *inkOnlyDocument, const QString &path, QString *why = nullptr);
};

#endif // PDFPAGESAVER_H
