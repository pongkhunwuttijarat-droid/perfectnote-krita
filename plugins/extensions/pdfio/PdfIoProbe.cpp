/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfIoProbe.h"

#include <cstdio>

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QTimer>

#if defined(PDFIO_HAVE_POPPLER)

#include "backends/poppler/PopplerRenderBackend.h"
#include "session/PdfPageSaver.h"
#include "session/PdfProjectBuilder.h"
#include "session/PdfSession.h"

#include <KisDocument.h>
#include <KisPart.h>

#include <kis_group_layer.h>
#include <kis_image.h>
#include <kis_paint_layer.h>

#include <KoColorSpaceConstants.h>

namespace PdfIoProbe {

void runIfRequested()
{
    const QString fixture = qEnvironmentVariable("PDFIO_PROBE");
    if (fixture.isEmpty()) {
        return;
    }

    PopplerRenderBackend backend;
    if (!backend.open(fixture)) {
        fprintf(stderr, "[probe] cannot open %s\n", qPrintable(fixture));
        return;
    }
    fprintf(stderr, "[probe] backend: pages %d\n", backend.pageCount());

    const QString projectDir = QDir(qEnvironmentVariable("PDFIO_PROBE_OUT", QStringLiteral("/tmp/pdfio-probe")))
                                   .filePath(QStringLiteral("project"));
    QDir().remove(projectDir);

    QString why;
    const PdfSessionManifest manifest = PdfSession::createProject(projectDir, fixture, backend, &why);
    if (!manifest.isValid(&why)) {
        fprintf(stderr, "[probe] session failed: %s\n", qPrintable(why));
        return;
    }
    fprintf(stderr, "[probe] session: pages %d, sha %s\n",
            manifest.pages.size(), qPrintable(QString::fromLatin1(manifest.sourceSha256.left(12))));

    const PdfSessionManifest reopened = PdfSession::openProject(projectDir, &why);
    fprintf(stderr, "[probe] reopen: %s\n", reopened.isValid() ? "ok" : qPrintable(why));

    /// The rotated page, the case that used to be interesting.
    KisImageSP image = PdfProjectBuilder::buildPageImage(manifest.pages.at(1), backend, 200.0, &why);
    if (!image) {
        fprintf(stderr, "[probe] builder failed: %s\n", qPrintable(why));
        return;
    }

    KisNodeSP background = image->root()->at(0);
    KisNodeSP inkGroup = image->root()->at(1);
    fprintf(stderr, "[probe] image %dx%d at %g dpi | layers %d | bg \"%s\" locked %d | ink \"%s\" children %d\n",
            image->width(), image->height(), image->xRes(), int(image->root()->childCount()),
            qPrintable(background->name()), int(background->userLocked()),
            qPrintable(inkGroup->name()), int(inkGroup->childCount()));

    /// The file size bound: save a document holding only the ink, never the page.
    KisDocument *inkOnly = PdfPageSaver::createInkOnlyDocument(image, &why);
    if (!inkOnly) {
        fprintf(stderr, "[probe] ink only document failed: %s\n", qPrintable(why));
        return;
    }
    fprintf(stderr, "[probe] ink only document: layers %d\n",
            inkOnly->image() ? int(inkOnly->image()->root()->childCount()) : -1);

    const QString path = QDir(projectDir).filePath(PdfSession::pageFileName(manifest.pages.at(1).index));
    QDir().mkpath(QFileInfo(path).absolutePath());

    QEventLoop loop;
    bool finished = false;
    QObject::connect(inkOnly, &KisDocument::sigSavingFinished, &loop,
                     [&loop, &finished](const QString &) { finished = true; loop.quit(); });
    QTimer::singleShot(60000, &loop, [&loop]() { loop.quit(); });

    if (!PdfPageSaver::saveInkOnly(inkOnly, path, &why)) {
        fprintf(stderr, "[probe] save failed: %s\n", qPrintable(why));
        return;
    }
    loop.exec();

    QFile saved(path);
    QByteArray raw;
    if (saved.open(QIODevice::ReadOnly)) {
        raw = saved.readAll();
    }
    fprintf(stderr, "[probe] ink only saved: finished %d, %lld bytes, zip %d, mergedimage %d\n",
            int(finished), qint64(raw.size()), int(raw.startsWith(QByteArrayLiteral("PK"))),
            int(raw.contains("mergedimage.png")));

    KisPart::instance()->removeDocument(inkOnly, true);
}

} // namespace PdfIoProbe

#else

namespace PdfIoProbe {
void runIfRequested()
{
    /// Desktop only: Android has no Poppler, and its renderer is a separate spike.
}
} // namespace PdfIoProbe

#endif // PDFIO_HAVE_POPPLER
