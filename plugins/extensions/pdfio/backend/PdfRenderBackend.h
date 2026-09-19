/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFRENDERBACKEND_H
#define PDFRENDERBACKEND_H

#include <QImage>
#include <QSizeF>
#include <QString>
#include <memory>

/**
 * One PDF page, as the session needs to reason about it.
 *
 * The backend is expected to hand back the page **as it should be displayed**, not as it
 * is stored: Poppler already applies /Rotate and a non-zero MediaBox origin, so nothing
 * downstream has to correct for either. That was measured, not assumed, in
 * pdfprobe/ and is recorded in docs/PDFIO-DESIGN.md.
 */
struct PdfPageInfo {
    int index = -1;
    /// Size of the displayed page in points, already rotated.
    QSizeF sizePt;
    /// The /Rotate the file declares, for the manifest. 0, 90, 180 or 270.
    int rotation = 0;

    bool isValid() const { return index >= 0 && sizePt.width() > 0 && sizePt.height() > 0; }
};

/**
 * Renders PDF pages for the pdfio session.
 *
 * Desktop uses Poppler, which Krita already depends on; Android has no Poppler at all and
 * goes through android.graphics.pdf.PdfRenderer with QJniObject instead. Everything above
 * this interface is platform neutral, which is the point of having it.
 */
class PdfRenderBackend
{
public:
    virtual ~PdfRenderBackend() = default;

    virtual bool open(const QString &path) = 0;
    virtual bool isOpen() const = 0;
    virtual int pageCount() const = 0;
    virtual PdfPageInfo pageInfo(int index) const = 0;

    /**
     * The page as a raster at the given resolution. Read the size of the returned image
     * rather than deriving it from sizePt: point-to-pixel rounding differs between
     * renderers (pdftoppm gave 834 px where Poppler Qt gave 833 for the same page).
     */
    virtual QImage renderPage(int index, qreal dpi) const = 0;

    /**
     * The selectable text of a page.
     *
     * Used to check that an export did not turn the source into a picture: the exporter never
     * rewrites an original object, so the text has to come back out.
     */
    virtual QString pageText(int index) const = 0;
};

#endif // PDFRENDERBACKEND_H
