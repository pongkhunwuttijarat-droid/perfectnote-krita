/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "AndroidDocumentPicker.h"
#include "PdfIoDocker.h"
#include "PdfIoPlugin.h"
#include "PdfIoProbe.h"
#include "PdfPageNavigator.h"
#include "PdfRendererSpike.h"

#include <cstdio>
#include <unistd.h>

#include <QDebug>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenu>
#include <QMenuBar>
#include <QTimer>

#if defined(PDFIO_HAVE_POPPLER)
#include "session/PdfPageSaver.h"
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

void say(const QString &message)
{
    /// Both sinks: see the note in PdfIoProbe about desktop versus Android.
    fprintf(stderr, "[pdfio] %s\n", qPrintable(message));
    fflush(stderr);
    qWarning("[pdfio] %s", qPrintable(message));
}

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

PdfIoPlugin::PdfIoPlugin(QObject *parent, const QVariantList &)
    : KisActionPlugin(parent)
{
    registerActions();

    /// Once per process: a view plugin is created for every view.
    registerPdfIoDocker();

    /// Temporary: answers whether the Android render backend can be pure C++.
    PdfRendererSpike::run();

    QString probePath = qEnvironmentVariable("PDFIO_PROBE");
#if defined(Q_OS_ANDROID)
    /// Temporary: adb cannot hand an environment variable to an Android application, so on
    /// Android the probe always runs, against the fixture it writes for itself.
    probePath = QStringLiteral("__builtin__");
#endif
    if (probePath.isEmpty()) {
        return;
    }

#if !defined(Q_OS_ANDROID)
    /// On desktop Krita's own message handler swallows plugin output during startup, so the probe
    /// routes everything to stderr. Not on Android, where stderr goes nowhere and Krita's Android
    /// log handler is the thing that reaches logcat -- installing this there swallowed the very
    /// output it was meant to reveal.
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        fprintf(stderr, "[probe] %s\n", qPrintable(message));
        fflush(stderr);
    });
#endif

    PdfIoProbe::runIfRequested();

    /// Deferred on purpose. Opening a document touches the main window, and from the plugin
    /// constructor during startup that window is still being built: the welcome screen and the
    /// toolbar handler are not ready. The real action is triggered long after startup, so queueing
    /// the probe the same way is both the fix and a faithful stand-in.
    QTimer::singleShot(0, this, [this, probePath]() {
#if defined(Q_OS_ANDROID)
        /// Temporary, and deliberately not the picker: this drives the very same open path with a
        /// file that is already on the device, so the crash reproduces unattended and the step
        /// logging in the navigator can be read straight out of logcat.
        const QDir cache(QStandardPaths::writableLocation(QStandardPaths::TempLocation));

        /// The file that was picked last is the one that crashed, so it is opened first when it is
        /// still there. The fixture is the known-good control.
        const QStringList candidates = {
            cache.filePath(QStringLiteral("pdfio-picked.pdf")),
            cache.filePath(QStringLiteral("pdfio-fixture.pdf")),
        };

        for (const QString &candidate : candidates) {
            if (!QFileInfo::exists(candidate)) {
                continue;
            }
            QString why;
            say(QStringLiteral("opening %1 (%2 bytes) through the real path")
                    .arg(candidate).arg(QFileInfo(candidate).size()));
            const bool ok = PdfPageNavigator::instance()->openNotebook(candidate, &why);
            say(QStringLiteral("openNotebook = %1 (%2)").arg(ok).arg(why));
        }
        return;
#endif
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

void PdfIoPlugin::registerActions()
{
    if (!viewManager() || !viewManager()->actionManager()) {
        return;
    }

    struct Entry {
        const char *name;
        void (PdfIoPlugin::*slot)();
    };

    const Entry entries[] = {
        { "pdfio_open_notebook", &PdfIoPlugin::slotOpenNotebook },
        { "pdfio_save_page", &PdfIoPlugin::slotSavePage },
        { "pdfio_next_page", &PdfIoPlugin::slotNextPage },
        { "pdfio_previous_page", &PdfIoPlugin::slotPreviousPage },
    };

    KisMainWindow *window = viewManager()->mainWindow();
    QMenu *menu = nullptr;
    if (window && window->menuBar()) {
        menu = window->menuBar()->findChild<QMenu *>(QStringLiteral("pdfio_menu"));
        if (!menu) {
            menu = window->menuBar()->addMenu(i18n("PDF Notebook"));
            menu->setObjectName(QStringLiteral("pdfio_menu"));
        }
    }

    for (const Entry &entry : entries) {
        KisAction *action = viewManager()->actionManager()->createAction(QString::fromLatin1(entry.name));
        if (!action) {
            continue;
        }
        connect(action, &KisAction::triggered, this, entry.slot);

        /// Creating an action does not put it anywhere. Without this the plugin is invisible.
        if (menu) {
            menu->addAction(action);
        }
    }
}

void PdfIoPlugin::slotOpenNotebook()
{
#if defined(Q_OS_ANDROID)
    /// QFileDialog is not usable here: the application has no broad filesystem access, so the
    /// file arrives as a content URI and has to be copied to a path the renderer can open.
    auto *picker = new AndroidDocumentPicker(this);
    say(QStringLiteral("the document picker is opening"));
    picker->pickPdf([this, picker](const QString &localPath, const QString &why) {
        picker->deleteLater();
        say(QStringLiteral("picker finished: path \"%1\" reason \"%2\"").arg(localPath, why));

        if (localPath.isEmpty()) {
            say(QStringLiteral("nothing was opened: %1").arg(why));
            return;
        }

        /// Deferred out of the activity result callback. Opening a document builds a view and
        /// walks the resource system, and doing that while the activity transition is still
        /// unwinding crashed inside Qt's own hash tables.
        QTimer::singleShot(0, this, [this, localPath]() {
            QString error;
            if (!PdfPageNavigator::instance()->openNotebook(localPath, &error)) {
                say(QStringLiteral("could not open the chosen file: %1").arg(error));
            }
        });
    });
#else
    const QString path = QFileDialog::getOpenFileName(nullptr,
                                                      i18n("Open PDF as notebook"),
                                                      QString(),
                                                      i18n("PDF documents (*.pdf)"));
    if (path.isEmpty()) {
        return;
    }

    if (!PdfPageNavigator::instance()->openNotebook(path, nullptr)) {
        qWarning() << "pdfio could not open" << path;
    }
#endif
}

void PdfIoPlugin::slotNextPage()
{
    QString why;
    if (!PdfPageNavigator::instance()->next(&why)) {
        qWarning() << "pdfio:" << why;
    }
}

void PdfIoPlugin::slotPreviousPage()
{
    QString why;
    if (!PdfPageNavigator::instance()->previous(&why)) {
        qWarning() << "pdfio:" << why;
    }
}

void PdfIoPlugin::slotSavePage()
{
#if defined(PDFIO_HAVE_POPPLER)
    KisDocument *document = viewManager() ? viewManager()->document() : nullptr;
    if (!document || !document->image()) {
        qWarning() << "pdfio: no page is open";
        return;
    }

    const QString projectDir = document->property("pdfioProjectDir").toString();
    const int pageIndex = document->property("pdfioPageIndex").toInt();
    if (projectDir.isEmpty()) {
        qWarning() << "pdfio: this document is not a note page";
        return;
    }

    QString why;
    KisDocument *inkOnly = PdfPageSaver::createInkOnlyDocument(document->image(), &why);
    if (!inkOnly) {
        qWarning() << "pdfio: cannot prepare the page:" << why;
        return;
    }

    const QString path = QDir(projectDir).filePath(PdfSession::pageFileName(pageIndex));
    QDir().mkpath(QFileInfo(path).absolutePath());

    /// Krita saves in the background, so the copy has to outlive this call. It is deleted when the
    /// save reports back, rather than by waiting here: a nested event loop around
    /// sigSavingFinished wedged on the second save.
    connect(inkOnly, &KisDocument::sigSavingFinished, this, [inkOnly, path](const QString &) {
        say(QStringLiteral("saved %1 (%2 bytes)").arg(path).arg(QFileInfo(path).size()));
        KisPart::instance()->removeDocument(inkOnly, true);
    });

    if (!PdfPageSaver::saveInkOnly(inkOnly, path, &why)) {
        qWarning() << "pdfio: cannot save:" << why;
        KisPart::instance()->removeDocument(inkOnly, true);
    }
#else
    qWarning() << "pdfio: saving needs a PDF backend";
#endif
}

bool PdfIoPlugin::openNotebook(const QString &pdfPath)
{
    QString why;
    const bool opened = PdfPageNavigator::instance()->openNotebook(pdfPath, &why);
    if (!opened) {
        say(QStringLiteral("openNotebook failed: %1").arg(why));
    }
    return opened;
}

void PdfIoPlugin::runScaleProbe(int pages)
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    QString why;

    say(QStringLiteral("scale: rss before %1 KB").arg(residentKb()));

    if (!navigator->openNotebook(qEnvironmentVariable("PDFIO_PROBE"), &why)) {
        say(QStringLiteral("scale: cannot open the notebook: %1").arg(why));
        return;
    }
    say(QStringLiteral("scale: page %1 rss %2 KB").arg(navigator->currentIndex() + 1).arg(residentKb()));

    /// Driven by a timer rather than a loop, and not for tidiness: closing a page defers the
    /// destruction of its view and document, so a tight loop frees nothing and the measurement
    /// would show growth that does not exist in use. A person also does not turn eight pages in
    /// the same millisecond.
    auto *timer = new QTimer(this);
    auto *turned = new int(1);

    connect(timer, &QTimer::timeout, this, [this, timer, turned, pages, navigator]() {
        QString why;
        if (*turned >= pages) {
            say(QStringLiteral("scale: done after %1 pages").arg(*turned));
            timer->stop();
            timer->deleteLater();
            delete turned;
            return;
        }

        if (!navigator->next(&why)) {
            say(QStringLiteral("scale: stopped after %1 pages: %2").arg(*turned).arg(why));
            timer->stop();
            timer->deleteLater();
            delete turned;
            return;
        }

        ++(*turned);
        say(QStringLiteral("scale: page %1 rss %2 KB").arg(navigator->currentIndex() + 1).arg(residentKb()));
    });

    timer->start(600);
}

#include "PdfIoPlugin.moc"
