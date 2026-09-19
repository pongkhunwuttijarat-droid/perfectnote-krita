/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFEXPORTER_H
#define PDFEXPORTER_H

#include "session/PdfSessionManifest.h"

#include <QHash>
#include <QImage>
#include <QString>

/**
 * Writes the source PDF again with the ink composited over it.
 *
 * The source is never rewritten: this is a PDF incremental update. Every original byte stays
 * where it was, and the overlay is added by appending a new content stream and *redefining the
 * existing page object* through the xref. That detail is the whole technique -- the page tree
 * already points at a fixed object number, so appending a brand new page object would simply
 * never be referenced. Because no original object is rewritten, the text of the source stays
 * extractable and the untouched pages stay untouched.
 *
 * The ink is placed in *page* space, so it rotates with the page rather than with the screen,
 * and a MediaBox that does not start at the origin needs no correction.
 *
 * Deliberately pure C++ with no renderer dependency, so the same code runs on desktop and on
 * Android, where there is no Poppler at all.
 */
class PdfExporter
{
public:
    /**
     * \a ink maps a page index to an image in displayed page space, the orientation the user
     * drew in. Pages without an entry are copied through untouched.
     */
    static bool exportWithInk(const QString &sourcePdf,
                              const PdfSessionManifest &manifest,
                              const QHash<int, QImage> &ink,
                              const QString &outPath,
                              QString *why = nullptr);

    /**
     * Object numbers of the pages, in order. Exposed for the test: an empty result means the
     * file keeps its objects in object streams, which this writer does not walk yet, and export
     * refuses rather than producing a file that only looks right.
     */
    static QList<int> pageObjectNumbers(const QByteArray &pdf, QString *why = nullptr);
};

#endif // PDFEXPORTER_H
