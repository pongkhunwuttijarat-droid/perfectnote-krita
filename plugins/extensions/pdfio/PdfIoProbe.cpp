/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfIoProbe.h"

#include <cstdio>

#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
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
#include <kis_paint_device.h>
#include <kis_paint_layer.h>

#include <KoColor.h>
#include <KoColorSpaceConstants.h>

namespace PdfIoProbe {

void runIfRequested()
{
    const QString fixture = qEnvironmentVariable("PDFIO_PROBE");
    if (fixture.isEmpty()) {
        return;
    }

    QTemporaryDir workspace;
    if (!workspace.isValid()) {
        qWarning() << "[pdfio] no temporary directory";
        return;
    }

    PopplerRenderBackend backend;
    if (!backend.open(fixture)) {
        qWarning() << "[pdfio] cannot open" << fixture;
        return;
    }
    qWarning() << "[pdfio] backend: pages" << backend.pageCount();

    QString why;

    /// 1. the project around the immutable source
    const QString projectDir = workspace.filePath(QStringLiteral("project"));
    const PdfSessionManifest manifest =
        PdfSession::createProject(projectDir, fixture, backend, &why);
    if (!manifest.isValid(&why)) {
        qWarning() << "[pdfio] session FAILED:" << why;
        return;
    }
    qWarning() << "[pdfio] session: ok, pages" << manifest.pages.size()
             << "source" << manifest.sourceFile
             << "sha" << manifest.sourceSha256.left(12);

    /// reopening has to accept the project it just wrote
    const PdfSessionManifest reopened = PdfSession::openProject(projectDir, &why);
    qWarning() << "[pdfio] reopen:" << (reopened.isValid() ? "ok" : "FAILED") << why;

    /// 2. the layer stack of the rotated page, the one that used to be the interesting case
    const PdfPageRecord &page = manifest.pages.at(1);
    KisImageSP image = PdfProjectBuilder::buildPageImage(page, backend, 200.0, &why);
    if (!image) {
        qWarning() << "[pdfio] builder FAILED:" << why;
        return;
    }

    KisNodeSP background = image->root()->at(0);
    KisNodeSP inkNode = image->root()->at(1);
    qWarning() << "[pdfio] image: ok" << image->width() << "x" << image->height()
             << "at" << image->xRes() << "dpi"
             << "| layers" << image->root()->childCount()
             << "| bottom" << background->name() << "locked" << background->userLocked()
             << "| top" << inkNode->name() << "locked" << inkNode->userLocked();

    /// 3. notes in the Ink group, then the ink-only save
    KisGroupLayer *ink = qobject_cast<KisGroupLayer *>(inkNode.data());
    if (!ink) {
        qWarning() << "[pdfio] the second layer is not a group";
        return;
    }

    KisPaintLayerSP notes = new KisPaintLayer(image, QStringLiteral("Stroke 1"), OPACITY_OPAQUE_U8);
    image->addNode(notes, ink);
    notes->paintDevice()->fill(QRect(20, 20, 40, 40), KoColor(Qt::black, image->colorSpace()));

    KisDocument *document = KisPart::instance()->createDocument();
    /// true: force the initial graph refresh. Without it the projection stays empty and the
    /// merged image written to the archive is a blank page, which understates what a naive save
    /// of a real session would cost.
    document->setCurrentImage(image, true);

    /// The save is deliberately not attempted. KraConverter reads doc->savingImage(), which
    /// KisDocument only fills in inside a real save operation and exposes no setter for, so
    /// calling it from here dereferences null. Recorded here because it is a real constraint
    /// on M4b rather than a detail: either KisDocument grows a small public lever, or the
    /// session drives the autosave path, which is the one place upstream already skips
    /// mergedimage.png.
    qWarning() << "[pdfio] document: created, savingImage null as expected:"
             << !document->savingImage()
             << "| image attached:" << bool(document->image());

    /// Which artifact is actually expensive? Save the editing document (page + ink) and an
    /// ink-only one, and compare. The design assumed the merged image was the problem; if the
    /// page is not in the saved document it may be cheap enough to stop fighting KraConverter.
    auto archiveReport = [](const QString &label, const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            fprintf(stderr, "[probe] %s: could not read\n", qPrintable(label));
            return;
        }
        const QByteArray raw = file.readAll();
        fprintf(stderr, "[probe] %s: %lld bytes, zip %d, mergedimage %d\n",
                qPrintable(label), qint64(raw.size()), int(raw.startsWith(QByteArrayLiteral("PK"))),
                int(raw.contains("mergedimage.png")));
    };

    /// Krita saves in the background, so the archive is not on disk when saveAs() returns.
    /// Wait for sigSavingFinished, with a guard so a stuck save cannot hang the probe forever.
    auto saveAndWait = [](KisDocument *doc, const QString &path) {
        QEventLoop loop;
        bool finished = false;
        QObject::connect(doc, &KisDocument::sigSavingFinished, &loop,
                         [&loop, &finished](const QString &) { finished = true; loop.quit(); });
        QTimer::singleShot(60000, &loop, [&loop]() { loop.quit(); });

        if (!doc->saveAs(path, QByteArrayLiteral("application/x-krita"), false)) {
            return false;
        }
        loop.exec();
        return finished;
    };

    /// Written outside the temporary directory so the archives outlive the process and can be
    /// taken apart with unzip afterwards.
    const QString outDir = qEnvironmentVariable("PDFIO_PROBE_OUT", QStringLiteral("/tmp/pdfio-probe"));
    QDir().mkpath(outDir);

    const QString withPagePath = QDir(outDir).filePath(QStringLiteral("with-page.kra"));
    fprintf(stderr, "[probe] saveAs(with page): %d\n", int(saveAndWait(document, withPagePath)));
    archiveReport(QStringLiteral("with-page.kra"), withPagePath);

    KisImageSP inkOnly = new KisImage(0, image->width(), image->height(), image->colorSpace(),
                                      QStringLiteral("ink only"));
    inkOnly->setResolution(image->xRes(), image->yRes());
    KisPaintLayerSP onlyInk = new KisPaintLayer(inkOnly, QStringLiteral("Ink"), OPACITY_OPAQUE_U8);
    onlyInk->paintDevice()->fill(QRect(20, 20, 40, 40), KoColor(Qt::black, inkOnly->colorSpace()));
    inkOnly->addNode(onlyInk, inkOnly->root());

    KisDocument *inkDocument = KisPart::instance()->createDocument();
    inkDocument->setCurrentImage(inkOnly, false);
    const QString inkOnlyPath = QDir(outDir).filePath(QStringLiteral("ink-only.kra"));
    fprintf(stderr, "[probe] saveAs(ink only): %d\n", int(saveAndWait(inkDocument, inkOnlyPath)));
    archiveReport(QStringLiteral("ink-only.kra"), inkOnlyPath);
    KisPart::instance()->removeDocument(inkDocument, true);

    /// Optional: build the whole project the way a real notebook would, one ink-only artifact
    /// per page, and report what it weighs on disk. This is the only way to get a real answer:
    /// the fixed costs per artifact (ICC profiles, preview, merged image, document xml) only
    /// show up when there are many of them.
    if (qEnvironmentVariableIsSet("PDFIO_PROBE_LOOP")) {
        const QString pagesDir = QDir(projectDir).filePath(QStringLiteral("pages"));
        qint64 total = 0;

        for (int i = 0; i < manifest.pages.size(); ++i) {
            QString pageWhy;
            KisImageSP pageImage =
                PdfProjectBuilder::buildPageImage(manifest.pages.at(i), backend, 200.0, &pageWhy);
            if (!pageImage) {
                fprintf(stderr, "[probe] page %d FAILED: %s\n", i + 1, qPrintable(pageWhy));
                continue;
            }

            KisGroupLayer *inkGroup = qobject_cast<KisGroupLayer *>(pageImage->root()->at(1).data());
            KisPaintLayerSP inkLayer = new KisPaintLayer(pageImage, QStringLiteral("Ink"), OPACITY_OPAQUE_U8);
            pageImage->addNode(inkLayer, inkGroup);

            /// A hundred strokes the size of a line of handwriting, spread over the page.
            for (int stroke = 0; stroke < 100; ++stroke) {
                const int x = 60 + (stroke / 25) * 480;
                const int y = 80 + (stroke % 25) * 90;
                inkLayer->paintDevice()->fill(QRect(x, y, 400, 6),
                                              KoColor(Qt::black, pageImage->colorSpace()));
            }

            /// What gets persisted is a document holding that ink and nothing else.
            KisImageSP inkOnlyPage = new KisImage(0, pageImage->width(), pageImage->height(),
                                                  pageImage->colorSpace(), QStringLiteral("ink"));
            inkOnlyPage->setResolution(200.0, 200.0);
            KisPaintLayerSP inkCopy = new KisPaintLayer(inkOnlyPage, QStringLiteral("Ink"), OPACITY_OPAQUE_U8);
            inkCopy->paintDevice()->makeCloneFrom(inkLayer->paintDevice(), pageImage->bounds());
            inkOnlyPage->addNode(inkCopy, inkOnlyPage->root());

            KisDocument *pageDocument = KisPart::instance()->createDocument();
            pageDocument->setCurrentImage(inkOnlyPage, false);
            const QString pagePath = QDir(pagesDir).filePath(
                QStringLiteral("p%1.kra").arg(i + 1, 4, 10, QLatin1Char('0')));
            saveAndWait(pageDocument, pagePath);
            KisPart::instance()->removeDocument(pageDocument, true);

            const qint64 pageBytes = QFileInfo(pagePath).size();
            total += pageBytes;
            if (i < 3 || i + 1 == manifest.pages.size()) {
                fprintf(stderr, "[probe] page %d/%d: %lld bytes\n",
                        i + 1, manifest.pages.size(), pageBytes);
            }
        }

        fprintf(stderr, "[probe] project pages: %lld bytes total, %lld average per page\n",
                total, total / qMax(1, manifest.pages.size()));
    }

    KisPart::instance()->removeDocument(document, true);

    qWarning() << "[pdfio] probe done";
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
