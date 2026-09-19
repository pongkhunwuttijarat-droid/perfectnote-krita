/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFINKLOADER_H
#define PDFINKLOADER_H

#include <QImage>
#include <QString>

/**
 * Reads the ink back out of a saved page artifact.
 *
 * The export needs the ink as an image, and going through Krita to get it would mean opening a
 * document per page and waiting for each one, which is exactly the round trip the project format
 * exists to avoid. A .kra is a zip and a paint layer is a PNG inside it, so the ink can be read
 * directly.
 */
class PdfInkLoader
{
public:
    /**
     * The ink of \a kraPath, or a null image when the page has none. \a why is set only on a
     * real failure, not for a page that was simply never drawn on.
     */
    static QImage loadInk(const QString &kraPath, QString *why = nullptr);

private:
    PdfInkLoader() = delete;
};

#endif // PDFINKLOADER_H
