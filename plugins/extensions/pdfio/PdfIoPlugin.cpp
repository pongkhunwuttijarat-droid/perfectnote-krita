/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfIoPlugin.h"
#include "PdfIoProbe.h"
#include "PdfRendererSpike.h"

#include <cstdio>

#include <QDebug>
#include <QFileInfo>

#if defined(PDFIO_HAVE_POPPLER)
#include "backends/poppler/PopplerRenderBackend.h"
#endif

#include <kpluginfactory.h>

namespace {

/**
 * Temporary: lets the desktop backend be exercised without a UI. Run Krita with
 * PDFIO_PROBE=<file.pdf> and the geometry and raster size of every page is logged, which is
 * what M4a has to get right before any layer is created.
 */
void probeDesktopBackendIfRequested()
{
#if defined(PDFIO_HAVE_POPPLER)
    const QString path = qEnvironmentVariable("PDFIO_PROBE");
    if (path.isEmpty()) {
        return;
    }

    PopplerRenderBackend backend;
    if (!backend.open(path)) {
        qWarning() << "[pdfio] could not open" << path;
        return;
    }

    qWarning() << "[pdfio] opened" << QFileInfo(path).fileName()
             << "pages" << backend.pageCount();
    for (int i = 0; i < backend.pageCount(); ++i) {
        const PdfPageInfo info = backend.pageInfo(i);
        const QImage image = backend.renderPage(i, 200.0);
        qWarning() << "[pdfio] page" << i + 1
                 << "sizePt" << info.sizePt << "rotate" << info.rotation
                 << "render" << image.size();
    }
#endif
}

} // namespace

K_PLUGIN_FACTORY_WITH_JSON(PdfIoPluginFactory, "kritapdfio.json", registerPlugin<PdfIoPlugin>();)

PdfIoPlugin::PdfIoPlugin(QObject *parent, const QVariantList &)
    : KisActionPlugin(parent)
{
    if (!qEnvironmentVariable("PDFIO_PROBE").isEmpty()) {
        /// Temporary: Krita's own message handler swallows plugin output during startup, so
        /// the probe routes everything straight to stderr while it is running.
        qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
            fprintf(stderr, "[probe] %s\n", qPrintable(message));
            fflush(stderr);
        });
        fprintf(stderr, "[probe] plugin constructed, probe requested\n");
    }

    /// Temporary: answers whether the Android render backend can be pure C++.
    PdfRendererSpike::run();
    /// Temporary: exercises the desktop backend on demand.
    probeDesktopBackendIfRequested();
    /// Temporary: runs the whole open and save path from inside the application.
    PdfIoProbe::runIfRequested();
}

PdfIoPlugin::~PdfIoPlugin()
{
}

#include "PdfIoPlugin.moc"
