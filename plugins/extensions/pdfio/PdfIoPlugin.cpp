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
#include <QScrollBar>
#include <QTimer>

/// Not behind PDFIO_HAVE_POPPLER: the session, the saver, the ink loader and the exporter are all
/// plain C++ and are built on every platform. Only the renderer differs, and that is chosen by
/// PdfRenderBackend::create.
#include "session/PdfExporter.h"
#include "session/PdfInkLoader.h"
#include "session/PdfPageSaver.h"
#include "session/PdfProjectBuilder.h"
#include "session/PdfSession.h"

#include <QHash>
#include <QImage>

#include <KoDocumentInfo.h>

#include <kis_canvas2.h>
#include <kis_coordinates_converter.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>

#include <KisView.h>

#include <KoColor.h>

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
        /// Android is driven by the menu action. The unattended route that opened a file from the
        /// cache at startup is gone: it existed to reproduce the open path crash, and it found it.
        /// Opening a document automatically on every launch would only surprise the user now.
        if (qEnvironmentVariableIntValue("PDFIO_PROBE_THUMBS") > 0) {
            runThumbnailProbe();
            return;
        }
        if (qEnvironmentVariableIntValue("PDFIO_PROBE_PAN") > 0) {
            runPanProbe();
            return;
        }
        if (qEnvironmentVariableIntValue("PDFIO_PROBE_RESTORE") > 0) {
            runRestoreProbe();
            return;
        }
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
        { "pdfio_export_pdf", &PdfIoPlugin::slotExportPdf },
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

    /// A switch rather than a plain action: turning pages by panning is the same gesture as
    /// looking at the bottom of a page, and whoever reads that way will want it off.
    KisAction *followAction =
        viewManager()->actionManager()->createAction(QStringLiteral("pdfio_follow_scrolling"));
    if (followAction) {
        followAction->setCheckable(true);
        followAction->setChecked(PdfPageNavigator::instance()->scrollFollowEnabled());
        connect(followAction, &KisAction::toggled, this, [](bool enabled) {
            PdfPageNavigator::instance()->setScrollFollowEnabled(enabled);
        });
        if (menu) {
            menu->addSeparator();
            menu->addAction(followAction);
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

void PdfIoPlugin::slotExportPdf()
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    if (!navigator->hasNotebook()) {
        qWarning() << "pdfio: no notebook is open";
        return;
    }

#if defined(Q_OS_ANDROID)
    /// Android has no useful file dialog: the export is written to a temporary file and then handed
    /// to the system's document creator, which is where the user picks the real destination.
    const QString target = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                               .filePath(QStringLiteral("pdfio-export.pdf"));
#else
    const QString target = QFileDialog::getSaveFileName(nullptr,
                                                        i18n("Export the notebook to PDF"),
                                                        QStringLiteral("notebook.pdf"),
                                                        i18n("PDF documents (*.pdf)"));
    if (target.isEmpty()) {
        return;
    }
#endif

    /// One image per page that was ever drawn on. The ink is read straight out of the saved
    /// artifacts, so exporting does not have to open a document per page.
    QHash<int, QImage> ink;
    const QDir project(navigator->projectDir());
    for (const PdfPageRecord &page : navigator->manifest().pages) {
        const QImage pageInk = PdfInkLoader::loadInk(project.filePath(page.kraFile), nullptr);
        if (!pageInk.isNull()) {
            ink.insert(page.index, pageInk);
        }
    }

    QString why;
    if (!PdfExporter::exportWithInk(navigator->sourcePath(), navigator->manifest(),
                                    ink, target, &why)) {
        say(QStringLiteral("export failed: %1").arg(why));
        return;
    }

    say(QStringLiteral("exported %1 of %2 pages with ink to %3")
            .arg(ink.size()).arg(navigator->pageCount()).arg(target));

#if defined(Q_OS_ANDROID)
    /// Off to wherever the user chooses. The temporary file is left behind on purpose: it is what
    /// the content resolver reads from, and the system may take its time getting there.
    auto *writer = new AndroidDocumentPicker(this);
    const QString suggested = QStringLiteral("%1-notes.pdf")
                                  .arg(QFileInfo(navigator->manifest().sourceFile).completeBaseName());
    writer->createPdf(suggested, target, [writer, this](bool written, const QString &why) {
        writer->deleteLater();
        if (!written) {
            say(QStringLiteral("the export was not saved: %1").arg(why));
            return;
        }
        say(QStringLiteral("the export was saved to the location that was chosen"));
    });
#endif
}

void PdfIoPlugin::slotSavePage()
{
    /// No platform guard: the saver is plain C++ and works wherever a render backend does.
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

void PdfIoPlugin::runRestoreProbe()
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    QString why;

    if (!navigator->openNotebook(qEnvironmentVariable("PDFIO_PROBE"), &why)) {
        say(QStringLiteral("restore: cannot open the notebook: %1").arg(why));
        return;
    }

    /// Draw a mark into the Ink layer of whatever page is open.
    KisDocument *opened = navigator->currentDocument();
    if (!opened) {
        say(QStringLiteral("restore: no document is open"));
        return;
    }

    KisImageSP image = opened->image();
    KisPaintLayer *stroke = qobject_cast<KisPaintLayer *>(PdfProjectBuilder::inkStrokeLayer(image).data());
    if (!stroke) {
        say(QStringLiteral("restore: no paintable Ink layer"));
        return;
    }
    stroke->paintDevice()->fill(QRect(100, 100, 200, 40), KoColor(Qt::black, image->colorSpace()));
    say(QStringLiteral("restore: drew a mark, ink bounds now %1,%2 %3x%4")
            .arg(stroke->paintDevice()->exactBounds().x())
            .arg(stroke->paintDevice()->exactBounds().y())
            .arg(stroke->paintDevice()->exactBounds().width())
            .arg(stroke->paintDevice()->exactBounds().height()));

    Q_UNUSED(why);

    /// Each turn on a tick of its own, the way a person takes them -- and not only for realism:
    /// the page left behind is closed on a later turn of the event loop, so turning twice inside
    /// one call stacks three views and Krita's window handling wedges. The probe wedged there
    /// twice before this was understood.
    QTimer::singleShot(600, this, [this, navigator]() {
        QString why;
        if (!navigator->next(&why)) {
            say(QStringLiteral("restore: cannot turn forward: %1").arg(why));
            return;
        }
        say(QStringLiteral("restore: turned to page %1").arg(navigator->currentIndex() + 1));

        QTimer::singleShot(600, this, [this, navigator]() {
            QString why;
            if (!navigator->previous(&why)) {
                say(QStringLiteral("restore: cannot turn back: %1").arg(why));
                return;
            }
            say(QStringLiteral("restore: turned back to page %1").arg(navigator->currentIndex() + 1));

            QTimer::singleShot(600, this, [navigator]() {
                /// Not a ternary: document->image() hands back a weak pointer, and a conditional
                /// cannot mix that with a strong one.
                KisDocument *returned = navigator->currentDocument();
                if (!returned) {
                    say(QStringLiteral("restore: nothing is open after turning back"));
                    return;
                }
                KisImageSP back = returned->image();
                KisPaintLayer *backStroke =
                    qobject_cast<KisPaintLayer *>(PdfProjectBuilder::inkStrokeLayer(back).data());
                if (!backStroke) {
                    say(QStringLiteral("restore: the returned page has no Ink layer"));
                    return;
                }

                const QRect bounds = backStroke->paintDevice()->exactBounds();
                say(QStringLiteral("restore: back on page %1, ink bounds %2,%3 %4x%5  (expected 100,100 200x40)")
                        .arg(navigator->currentIndex() + 1)
                        .arg(bounds.x()).arg(bounds.y())
                        .arg(bounds.width()).arg(bounds.height()));
            });
        });
    });
}

void PdfIoPlugin::runPanProbe()
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    QString why;
    if (!navigator->openNotebook(qEnvironmentVariable("PDFIO_PROBE"), &why)) {
        say(QStringLiteral("pan: cannot open the notebook: %1").arg(why));
        return;
    }

    QTimer::singleShot(900, this, [navigator]() {
        KisView *view = navigator->currentView();
        KisDocument *document = navigator->currentDocument();
        if (!view || !view->canvasBase() || !document || !document->image()) {
            say(QStringLiteral("pan: no canvas to measure"));
            return;
        }

        KisCanvas2 *canvas = view->canvasBase();
        const KisCoordinatesConverter *converter = canvas->coordinatesConverter();
        QWidget *widget = canvas->canvasWidget();
        if (!converter || !widget) {
            say(QStringLiteral("pan: no converter"));
            return;
        }

        const QRectF pageRect(0, 0, document->image()->width(), document->image()->height());

        QScrollBar *vertical = nullptr;
        QScrollBar *horizontal = nullptr;
        const QList<QScrollBar *> bars = widget->findChildren<QScrollBar *>();
        for (QScrollBar *bar : bars) {
            say(QStringLiteral("pan: %1 scrollbar range %2..%3 value %4 pageStep %5 widget %6x%7")
                    .arg(bar->orientation() == Qt::Vertical ? QStringLiteral("vertical")
                                                            : QStringLiteral("horizontal"))
                    .arg(bar->minimum()).arg(bar->maximum()).arg(bar->value())
                    .arg(bar->pageStep()).arg(widget->width()).arg(widget->height()));
            if (bar->orientation() == Qt::Vertical) {
                vertical = bar;
            } else {
                horizontal = bar;
            }
        }

        const QRectF pageOnScreen = converter->documentToWidget(pageRect);
        say(QStringLiteral("pan: page on screen %1,%2 %3x%4, viewport %5x%6")
                .arg(pageOnScreen.x()).arg(pageOnScreen.y())
                .arg(pageOnScreen.width()).arg(pageOnScreen.height())
                .arg(widget->width()).arg(widget->height()));

        if (vertical) {
            vertical->setValue(vertical->maximum());
            const QRectF atBottom = converter->documentToWidget(pageRect);
            say(QStringLiteral("pan: at the very bottom the page bottom sits at %1 of %2")
                    .arg(atBottom.bottom()).arg(widget->height()));
        }
        if (horizontal) {
            horizontal->setValue(horizontal->maximum());
            const QRectF atRight = converter->documentToWidget(pageRect);
            say(QStringLiteral("pan: at the far right the page right sits at %1 of %2")
                    .arg(atRight.right()).arg(widget->width()));
        }
    });
}

void PdfIoPlugin::runThumbnailProbe()
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    QString why;
    if (!navigator->openNotebook(qEnvironmentVariable("PDFIO_PROBE"), &why)) {
        say(QStringLiteral("thumbs: cannot open the notebook: %1").arg(why));
        return;
    }

    const QDir project(navigator->projectDir());
    for (int i = 0; i < navigator->pageCount(); ++i) {
        navigator->ensureThumbnail(i);
    }

    /// Time for the queue, which works one page at a time on purpose.
    QTimer::singleShot(3000, this, [navigator, project]() {
        for (int i = 0; i < navigator->pageCount(); ++i) {
            const QFileInfo info(project.filePath(navigator->manifest().pages.at(i).thumbFile));
            say(QStringLiteral("thumbs: page %1 %2 (%3 bytes)")
                    .arg(i + 1)
                    .arg(info.exists() ? QStringLiteral("written") : QStringLiteral("MISSING"))
                    .arg(info.size()));
        }
    });
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
