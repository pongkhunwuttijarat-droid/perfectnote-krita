/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfIoPlugin.h"
#include "PdfIoProbe.h"
#include "PdfRendererSpike.h"

#include <cstdio>
#include <unistd.h>

#include <QDebug>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenu>
#include <QMenuBar>
#include <QStandardPaths>
#include <QTimer>

#if defined(PDFIO_HAVE_POPPLER)
#include "backends/poppler/PopplerRenderBackend.h"
#include "session/PdfProjectBuilder.h"
#include "session/PdfSession.h"
#endif

#include <KoDocumentInfo.h>

#include <KisDocument.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisViewManager.h>
#include <kis_action.h>
#include <kis_action_manager.h>
#include <klocalizedstring.h>
#include <kpluginfactory.h>

K_PLUGIN_FACTORY_WITH_JSON(PdfIoPluginFactory, "kritapdfio.json", registerPlugin<PdfIoPlugin>();)

namespace {

/// Where a notebook lives: one directory per source PDF, under the application data location.
QString projectRoot()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("pdfio-projects"));
}

void say(const QString &message)
{
    fprintf(stderr, "[pdfio] %s\n", qPrintable(message));
    fflush(stderr);
}

} // namespace

PdfIoPlugin::PdfIoPlugin(QObject *parent, const QVariantList &)
    : KisActionPlugin(parent)
{
    registerActions();

    /// Temporary: answers whether the Android render backend can be pure C++.
    PdfRendererSpike::run();

    const QString probePath = qEnvironmentVariable("PDFIO_PROBE");
    if (probePath.isEmpty()) {
        return;
    }

    /// Krita's own message handler swallows plugin output during startup, so route everything
    /// to stderr while the probe runs, and exercise the same entry point the action uses.
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        fprintf(stderr, "[probe] %s\n", qPrintable(message));
        fflush(stderr);
    });

    PdfIoProbe::runIfRequested();

    /// Deferred on purpose. Opening a document touches the main window, and from the plugin
    /// constructor during startup that window is still being built: addViewAndNotifyLoadingCompleted
    /// hides the welcome screen and walks the toolbar handler, whose action list is empty this
    /// early. The real action is triggered by the user long after startup, so queueing the probe
    /// the same way is both the fix and the faithful test.
    QTimer::singleShot(0, this, [this, probePath]() {
        const int scale = qEnvironmentVariableIntValue("PDFIO_PROBE_SCALE");
        if (scale > 0) {
            runScaleProbe(scale);
            return;
        }
        const bool opened = openNotebook(probePath);
        say(QStringLiteral("openNotebook(%1) = %2").arg(probePath).arg(opened));
    });
}

PdfIoPlugin::~PdfIoPlugin()
{
}

namespace {

/// Resident set size in kilobytes, from /proc: the number that decides whether a notebook can
/// stay open on a tablet.
qint64 residentKb()
{
    QFile statm(QStringLiteral("/proc/self/statm"));
    if (!statm.open(QIODevice::ReadOnly)) {
        return -1;
    }
    const QList<QByteArray> fields = statm.readAll().simplified().split(' ');
    if (fields.size() < 2) {
        return -1;
    }
    return fields.at(1).toLongLong() * (sysconf(_SC_PAGESIZE) / 1024);
}

} // namespace

void PdfIoPlugin::runScaleProbe(int pages)
{
#if defined(PDFIO_HAVE_POPPLER)
    const QString pdfPath = qEnvironmentVariable("PDFIO_PROBE");
    PopplerRenderBackend backend;
    if (!backend.open(pdfPath)) {
        say(QStringLiteral("scale: cannot open %1").arg(pdfPath));
        return;
    }

    const QString root = projectRoot();
    QDir().mkpath(root);
    const QString base = QFileInfo(pdfPath).completeBaseName();
    const QString key = QString::fromLatin1(PdfSessionManifest::sha256OfFile(pdfPath).left(8));
    const QString projectDir = QDir(root).filePath(base + QLatin1Char('-') + key);

    QString why;
    const PdfSessionManifest manifest =
        QFileInfo::exists(PdfSession::manifestPath(projectDir))
            ? PdfSession::openProject(projectDir, &why)
            : PdfSession::createProject(projectDir, pdfPath, backend, &why);
    if (!manifest.isValid(&why)) {
        say(QStringLiteral("scale: project failed: %1").arg(why));
        return;
    }

    say(QStringLiteral("scale: pages available %1, rss before %2 KB")
            .arg(manifest.pages.size())
            .arg(residentKb()));

    const int count = qMin(pages, manifest.pages.size());
    for (int i = 0; i < count; ++i) {
        KisImageSP image = PdfProjectBuilder::buildPageImage(manifest.pages.at(i), backend, 200.0, &why);
        if (!image) {
            say(QStringLiteral("scale: page %1 failed: %2").arg(i + 1).arg(why));
            continue;
        }

        KisDocument *document = KisPart::instance()->createDocument();
        document->setCurrentImage(image, true, PdfProjectBuilder::inkStrokeLayer(image));
        KisPart::instance()->addDocument(document);

        if (KisMainWindow *window = viewManager() ? viewManager()->mainWindow() : nullptr) {
            window->addViewAndNotifyLoadingCompleted(document);
        }

        say(QStringLiteral("scale: page %1/%2 rss %3 KB documents %4")
                .arg(i + 1).arg(count).arg(residentKb()).arg(KisPart::instance()->documentCount()));
    }
#else
    Q_UNUSED(pages);
#endif
}

void PdfIoPlugin::registerActions()
{
    if (!viewManager() || !viewManager()->actionManager()) {
        return;
    }

    KisAction *openAction = viewManager()->actionManager()->createAction(QStringLiteral("pdfio_open_notebook"));
    if (openAction) {
        connect(openAction, &KisAction::triggered, this, &PdfIoPlugin::slotOpenNotebook);
    }

    KisAction *saveAction = viewManager()->actionManager()->createAction(QStringLiteral("pdfio_save_page"));
    if (saveAction) {
        connect(saveAction, &KisAction::triggered, this, &PdfIoPlugin::slotSavePage);
    }

    /// Creating an action does not put it anywhere. Without this the plugin is invisible: the
    /// actions exist in the collection and no menu ever shows them, which is exactly what
    /// "I don't see anything" looks like.
    KisMainWindow *window = viewManager()->mainWindow();
    if (!window || !window->menuBar()) {
        return;
    }

    QMenu *menu = window->menuBar()->findChild<QMenu *>(QStringLiteral("pdfio_menu"));
    if (!menu) {
        menu = window->menuBar()->addMenu(i18n("PDF Notebook"));
        menu->setObjectName(QStringLiteral("pdfio_menu"));
    }
    if (openAction) {
        menu->addAction(openAction);
    }
    if (saveAction) {
        menu->addAction(saveAction);
    }
}

void PdfIoPlugin::slotOpenNotebook()
{
    const QString path = QFileDialog::getOpenFileName(nullptr,
                                                      i18n("Open PDF as notebook"),
                                                      QString(),
                                                      i18n("PDF documents (*.pdf)"));
    if (path.isEmpty()) {
        return;
    }

    if (!openNotebook(path)) {
        qWarning() << "pdfio could not open" << path;
    }
}

void PdfIoPlugin::slotSavePage()
{
    /// Saving the page is the next step: it has to write an ink-only artifact, which means a
    /// document holding just the Ink group, and it has to be driven from the event loop rather
    /// than a nested one (a nested wait for sigSavingFinished wedged on the second save).
    qWarning() << "pdfio: saving a page is not wired up yet";
}

bool PdfIoPlugin::openNotebook(const QString &pdfPath)
{
#if defined(PDFIO_HAVE_POPPLER)
    if (!QFileInfo::exists(pdfPath)) {
        say(QStringLiteral("no such file: %1").arg(pdfPath));
        return false;
    }

    PopplerRenderBackend backend;
    if (!backend.open(pdfPath)) {
        say(QStringLiteral("the renderer cannot open %1").arg(pdfPath));
        return false;
    }

    const QString root = projectRoot();
    if (!QDir().mkpath(root)) {
        say(QStringLiteral("cannot create %1").arg(root));
        return false;
    }

    /// One directory per source PDF, keyed by its content, so reopening the same document
    /// returns to the same notebook instead of starting a second one.
    const QString base = QFileInfo(pdfPath).completeBaseName();
    const QString key = QString::fromLatin1(PdfSessionManifest::sha256OfFile(pdfPath).left(8));
    const QString projectDir = QDir(root).filePath(base + QLatin1Char('-') + key);

    QString why;
    const PdfSessionManifest manifest =
        QFileInfo::exists(PdfSession::manifestPath(projectDir))
            ? PdfSession::openProject(projectDir, &why)
            : PdfSession::createProject(projectDir, pdfPath, backend, &why);

    if (!manifest.isValid(&why)) {
        say(QStringLiteral("project failed: %1").arg(why));
        return false;
    }

    KisImageSP image = PdfProjectBuilder::buildPageImage(manifest.pages.first(), backend, 200.0, &why);
    if (!image) {
        say(QStringLiteral("page failed: %1").arg(why));
        return false;
    }

    KisDocument *document = KisPart::instance()->createDocument();
    document->documentInfo()->setAboutInfo(QStringLiteral("title"), base);
    /// Activate the paintable layer inside Ink, not the group: opening on the group would leave
    /// the user unable to draw even once the layer exists.
    document->setCurrentImage(image, true, PdfProjectBuilder::inkStrokeLayer(image));
    KisPart::instance()->addDocument(document);

    KisMainWindow *window = viewManager() ? viewManager()->mainWindow() : nullptr;
    if (window) {
        window->addViewAndNotifyLoadingCompleted(document);
    }

    say(QStringLiteral("opened %1: %2 pages, page %3 at %4x%5, project %6")
            .arg(base)
            .arg(manifest.pages.size())
            .arg(manifest.pages.first().index + 1)
            .arg(image->width())
            .arg(image->height())
            .arg(projectDir));
    return true;
#else
    Q_UNUSED(pdfPath);
    say(QStringLiteral("no PDF backend on this platform yet"));
    return false;
#endif
}

#include "PdfIoPlugin.moc"
