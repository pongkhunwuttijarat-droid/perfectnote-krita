/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfIoProbe.h"

#include <cstdio>

#include <QDebug>
#include <QFile>
#include <QTemporaryDir>

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
    document->setCurrentImage(image, false);

    /// The save is deliberately not attempted. KraConverter reads doc->savingImage(), which
    /// KisDocument only fills in inside a real save operation and exposes no setter for, so
    /// calling it from here dereferences null. Recorded here because it is a real constraint
    /// on M4b rather than a detail: either KisDocument grows a small public lever, or the
    /// session drives the autosave path, which is the one place upstream already skips
    /// mergedimage.png.
    qWarning() << "[pdfio] document: created, savingImage null as expected:"
             << !document->savingImage()
             << "| image attached:" << bool(document->image());

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
