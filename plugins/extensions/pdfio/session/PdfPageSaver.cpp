/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfPageSaver.h"

#include <QIODevice>

#include <kra_converter.h>

#include <KisDocument.h>
#include <KisImportExportErrorCode.h>

namespace {

void fail(QString *why, const QString &message)
{
    if (why) {
        *why = message;
    }
}

} // namespace

bool PdfPageSaver::saveInkOnly(KisDocument *document, QIODevice *io, const QString &fileName, QString *why)
{
    if (!document) {
        fail(why, QStringLiteral("no document to save"));
        return false;
    }
    if (!io || !io->isWritable()) {
        fail(why, QStringLiteral("the destination is not writable"));
        return false;
    }

    KraConverter converter(document);

    /// false: no merged image. This is the documented lever for the size budget, and it is
    /// the same value the KRA exporter passes while autosaving.
    const KisImportExportErrorCode result = converter.buildFile(io, fileName, false);
    if (!result.isOk()) {
        fail(why, QStringLiteral("saving %1 failed: %2").arg(fileName, result.errorMessage()));
        return false;
    }

    return true;
}
