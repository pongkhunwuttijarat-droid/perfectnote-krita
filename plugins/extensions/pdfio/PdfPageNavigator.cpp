/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfPageNavigator.h"

#include <cstdio>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QWidget>
#include <QStandardPaths>
#include <QTimer>

#include "backend/PdfRenderBackend.h"
#include "PdfPageStripDecoration.h"
#include "session/PdfStripBuilder.h"
#include "session/PdfInkLoader.h"
#include "session/PdfPageSaver.h"
#include "session/PdfProjectBuilder.h"
#include "session/PdfSession.h"

#include <KoDocumentInfo.h>

#include <kis_paint_device.h>
#include <kis_paint_layer.h>

#include <kis_canvas2.h>
#include <kis_coordinates_converter.h>

#include <kis_node_manager.h>

#include <KisDocument.h>
#include <KisMainWindow.h>
#include <KisViewManager.h>
#include <KisPart.h>
#include <KisView.h>

namespace {

void fail(QString *why, const QString &message)
{
    if (why) {
        *why = message;
    }
}

void say(const QString &message)
{
    /// Both sinks: see the note in PdfIoProbe. On Android stderr goes nowhere and it is qWarning,
    /// through Krita's Android log handler, that reaches logcat.
    fprintf(stderr, "[pdfio] %s\n", qPrintable(message));
    fflush(stderr);
    qWarning("[pdfio] %s", qPrintable(message));
}

/// One gesture, one page.
constexpr qint64 TurnCooldownMs = 700;

/// How long the view has to sit still before the page under it is opened. Long enough that a
/// gesture in progress never triggers it, short enough not to feel deliberate.
constexpr qint64 SettleMs = 450;

/// The gap the strip decoration leaves between pages, in widget pixels.
constexpr qreal GapWidgetPixels = 16;

/// How wide a generated thumbnail is. The docker shows it smaller still; the extra is there so it
/// stays sharp when the interface is scaled up.
constexpr int ThumbnailPixels = 256;

/// The coarsest a page is rendered at on its way to a thumbnail. Below this, text stops being
/// recognisable and the thumbnail stops being useful for choosing a page.
constexpr qreal ThumbnailRenderDpi = 96;

} // namespace

PdfPageNavigator *PdfPageNavigator::instance()
{
    static PdfPageNavigator navigator;
    return &navigator;
}

QString PdfPageNavigator::projectRoot()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("pdfio-projects"));
}

bool PdfPageNavigator::hasNotebook() const
{
    return !m_projectDir.isEmpty() && m_manifest.isValid();
}

bool PdfPageNavigator::scrollFollowEnabled() const
{
    return m_scrollFollow;
}

void PdfPageNavigator::setScrollFollowEnabled(bool enabled)
{
    if (m_scrollFollow == enabled) {
        return;
    }
    m_scrollFollow = enabled;
    say(QStringLiteral("turning pages by panning is now %1").arg(enabled ? "on" : "off"));
    Q_EMIT pageChanged(m_index, pageCount(), QFileInfo(m_manifest.sourceFile).completeBaseName());
}

void PdfPageNavigator::checkScrollFollow()
{
    if (!m_scrollFollow || !m_view || !m_view->canvasBase() || m_index < 0) {
        return;
    }

    KisCanvas2 *canvas = m_view->canvasBase();
    const KisCoordinatesConverter *converter = canvas->coordinatesConverter();
    QWidget *widget = canvas->canvasWidget();
    KisDocument *document = m_document;
    if (!converter || !widget || !document || !document->image()) {
        return;
    }

    const QRectF page = converter->documentToWidget(
        QRectF(QPointF(0, 0), QSizeF(document->image()->width(), document->image()->height())));
    if (page.isEmpty()) {
        return;
    }

    /// The zoom, read off the page's own on-screen width.
    const qreal zoom = page.width() / qMax(qreal(1), qreal(document->image()->width()));

    /// Which page the middle of the view is over, in document coordinates.
    ///
    /// This replaced a rule that watched how far the page had been panned past its own edge, and
    /// that rule was wrong. A page that merely sits low in the viewport -- which is what the
    /// headless canvas does, and what centring can do anywhere -- is indistinguishable, to such a
    /// rule, from a page pulled down past its top. It then turned pages nobody had scrolled, every
    /// time the timer fired, until flooding the log was the only thing the application was doing.
    const QPointF centre = converter->widgetToDocument(QPointF(widget->rect().center()));
    const int pageUnderCentre = pageAtDocumentPoint(centre, zoom);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    /// Act only once the view has stopped on that page. Turning one costs about 650 ms, measured,
    /// so it must not happen in the middle of a gesture.
    if (pageUnderCentre != m_candidatePage) {
        m_candidatePage = pageUnderCentre;
        m_candidateSince = now;
        return;
    }

    if (pageUnderCentre < 0 || pageUnderCentre == m_index) {
        return;
    }

    if (now - m_candidateSince < SettleMs || now - m_lastTurn < TurnCooldownMs) {
        return;
    }

    m_lastTurn = now;
    m_candidatePage = -1;

    QString why;
    if (!showPage(pageUnderCentre, &why)) {
        say(QStringLiteral("scroll: could not open page %1 (%2)").arg(pageUnderCentre + 1).arg(why));
    }
}

void PdfPageNavigator::ensureThumbnail(int index)
{
    if (!hasNotebook() || index < 0 || index >= m_manifest.pages.size()) {
        return;
    }

    const QString path = QDir(m_projectDir).filePath(m_manifest.pages.at(index).thumbFile);
    if (QFileInfo::exists(path)) {
        Q_EMIT thumbnailReady(index);
        return;
    }

    if (!m_thumbnailQueue.contains(index)) {
        m_thumbnailQueue.append(index);
    }

    if (!m_thumbnailTimer) {
        m_thumbnailTimer = new QTimer(this);
        connect(m_thumbnailTimer, &QTimer::timeout, this, &PdfPageNavigator::makeOneThumbnail);
    }
    if (!m_thumbnailTimer->isActive()) {
        /// One at a time, slow enough that the window keeps redrawing while a long notebook fills
        /// in.
        m_thumbnailTimer->start(40);
    }
}

void PdfPageNavigator::makeOneThumbnail()
{
    if (m_thumbnailQueue.isEmpty()) {
        m_thumbnailTimer->stop();
        return;
    }

    const int index = m_thumbnailQueue.takeFirst();
    if (index < 0 || index >= m_manifest.pages.size()) {
        return;
    }

    if (!m_thumbnailBackend) {
        m_thumbnailBackend.reset(PdfRenderBackend::create());
    }
    if (!m_thumbnailBackend || !m_thumbnailBackend->isOpen()) {
        if (!m_thumbnailBackend || !m_thumbnailBackend->open(sourcePath())) {
            return;
        }
    }

    /// Rendered coarser than the page but never as coarse as the thumbnail's own pixel count
    /// suggests. Asking for exactly 256 pixels of an A4 page means about 31 dpi, and text at 31 dpi
    /// is a grey smear: the thumbnail was unreadable. Rendering at 96 and shrinking costs a
    /// megapixel and looks like a page.
    const PdfPageInfo info = m_thumbnailBackend->pageInfo(index);
    const qreal widthPt = qMax(qreal(1), info.sizePt.width());
    const qreal dpi = qBound(ThumbnailRenderDpi, ThumbnailPixels * 72.0 / widthPt, qreal(200));

    const QImage page = m_thumbnailBackend->renderPage(index, dpi);
    if (page.isNull()) {
        return;
    }

    const QImage thumbnail = page.scaled(ThumbnailPixels, ThumbnailPixels,
                                         Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const QString path = QDir(m_projectDir).filePath(m_manifest.pages.at(index).thumbFile);
    QDir().mkpath(QFileInfo(path).absolutePath());
    if (!thumbnail.save(path, "PNG")) {
        return;
    }

    say(QStringLiteral("thumbnail for page %1 written (%2x%3)")
            .arg(index + 1).arg(thumbnail.width()).arg(thumbnail.height()));
    Q_EMIT thumbnailReady(index);
}

KisView *PdfPageNavigator::currentView() const
{
    return m_view;
}

int PdfPageNavigator::pageAtDocumentPoint(const QPointF &point, qreal zoom) const
{
    if (m_index < 0 || m_index >= m_manifest.pages.size() || !m_document || !m_document->image()) {
        return -1;
    }

    const QSizeF page(m_document->image()->width(), m_document->image()->height());
    const QRectF pageRect(0, 0, page.width(), page.height());
    if (pageRect.contains(point)) {
        return m_index;
    }

    /// The open page's own scale: the manifest speaks in points and the image in pixels.
    const qreal pageWidthPt = m_manifest.pages.at(m_index).sizePt.width();
    const qreal pixelsPerPoint = pageWidthPt > 0 ? page.width() / pageWidthPt : 1.0;

    /// The same arrangement the strip decoration draws: the neighbours directly above and below,
    /// each at its own size, separated by the gap.
    const qreal gap = GapWidgetPixels / qMax(qreal(0.0001), zoom);

    for (int direction : { -1, 1 }) {
        const int other = m_index + direction;
        if (other < 0 || other >= m_manifest.pages.size()) {
            continue;
        }

        const QSizeF neighbour = m_manifest.pages.at(other).sizePt * pixelsPerPoint;
        const qreal top = direction > 0 ? pageRect.bottom() + gap
                                        : pageRect.top() - gap - neighbour.height();

        if (QRectF(pageRect.left(), top, neighbour.width(), neighbour.height()).contains(point)) {
            return other;
        }
    }

    return -1;
}

KisDocument *PdfPageNavigator::currentDocument() const
{
    return m_document;
}

const PdfSessionManifest &PdfPageNavigator::manifest() const
{
    return m_manifest;
}

QString PdfPageNavigator::sourcePath() const
{
    return PdfSession::sourcePath(m_projectDir, m_manifest.sourceFile);
}

int PdfPageNavigator::pageCount() const
{
    return m_manifest.pages.size();
}

int PdfPageNavigator::currentIndex() const
{
    return m_index;
}

QString PdfPageNavigator::projectDir() const
{
    return m_projectDir;
}

bool PdfPageNavigator::openNotebook(const QString &pdfPath, QString *why)
{
    if (!QFileInfo::exists(pdfPath)) {
        fail(why, QStringLiteral("no such file: %1").arg(pdfPath));
        return false;
    }

    QScopedPointer<PdfRenderBackend> backend(PdfRenderBackend::create());
    if (!backend) {
        fail(why, QStringLiteral("no PDF render backend on this platform"));
        return false;
    }

    if (!backend->open(pdfPath)) {
        fail(why, QStringLiteral("the renderer cannot open %1").arg(pdfPath));
        return false;
    }

    const QString root = projectRoot();
    if (!QDir().mkpath(root)) {
        fail(why, QStringLiteral("cannot create %1").arg(root));
        return false;
    }

    /// One directory per source PDF, keyed by its content, so reopening returns to the same
    /// notebook instead of starting a second one.
    const QString base = QFileInfo(pdfPath).completeBaseName();
    const QString key = QString::fromLatin1(PdfSessionManifest::sha256OfFile(pdfPath).left(8));
    const QString projectDir = QDir(root).filePath(base + QLatin1Char('-') + key);

    const PdfSessionManifest manifest =
        QFileInfo::exists(PdfSession::manifestPath(projectDir))
            ? PdfSession::openProject(projectDir, why)
            : PdfSession::createProject(projectDir, pdfPath, *backend, why);

    if (!manifest.isValid(why)) {
        return false;
    }

    say(QStringLiteral("project ready: %1 pages at %2").arg(manifest.pages.size()).arg(projectDir));

    m_projectDir = projectDir;
    m_manifest = manifest;
    const bool shown = showPage(0, why);
    Q_EMIT pageChanged(m_index, pageCount(), base);

    /// Watched on a timer rather than from the canvas: panning arrives as wheel or touch events
    /// depending on the device, and where the page ended up afterwards is the same question either
    /// way.
    if (!m_scrollWatch) {
        m_scrollWatch = new QTimer(this);
        connect(m_scrollWatch, &QTimer::timeout, this, &PdfPageNavigator::checkScrollFollow);
    }
    m_scrollWatch->start(150);

    return shown;
}

void PdfPageNavigator::closeCurrentPage()
{
    /// The view takes the document with it, and removing the document as well leaves the view
    /// alive with nothing behind it. See the note in showPage.
    if (m_view) {
        m_view->closeView();
        m_view = nullptr;
        m_document = nullptr;
    } else if (m_document) {
        KisPart::instance()->removeDocument(m_document, true);
        m_document = nullptr;
    }
}

bool PdfPageNavigator::buildForStrip(int index, QString *why)
{
    QScopedPointer<PdfRenderBackend> backend(PdfRenderBackend::create());
    if (!backend || !backend->open(sourcePath())) {
        fail(why, QStringLiteral("the renderer cannot open the source"));
        return false;
    }

    say(QStringLiteral("building a strip of %1 around page %2").arg(m_scope).arg(index + 1));
    const PdfStripBuilder::Strip strip = PdfStripBuilder::build(m_manifest, index, m_scope, m_dpi,
                                                                *backend, m_projectDir, why);
    if (!strip.image) {
        return false;
    }

    QList<int> pages;
    for (const PdfStripLayout::Slot &slot : strip.layout.slots()) {
        pages.append(slot.page);
    }
    say(QStringLiteral("strip holds pages %1").arg(
        [&pages]() {
            QStringList names;
            for (int page : pages) {
                names.append(page < 0 ? QStringLiteral("-") : QString::number(page + 1));
            }
            return names.join(QLatin1Char(','));
        }()));

    Q_UNUSED(pages);
    return showImage(strip.image, strip.activeInkLayer, index, strip.layout, why);
}

bool PdfPageNavigator::activateWithinStrip(int index, QString *why)
{
    if (m_stripPages.isEmpty() || !m_document || !m_document->image()) {
        fail(why, QStringLiteral("no strip is open"));
        return false;
    }

    const int slot = m_stripPages.indexOf(index);
    if (slot < 0) {
        fail(why, QStringLiteral("page %1 is not in the strip").arg(index + 1));
        return false;
    }

    lockSlotsBut(slot, m_stripActiveSlot);

    /// And make it the node Krita is working on, so the brush goes to this page and the layer
    /// docker shows it. This is the same call the crash stack went through, reached deliberately
    /// rather than by accident.
    KisImageSP image = m_document->image();
    KisNodeSP layer;
    for (quint32 i = 0; i < image->root()->childCount(); ++i) {
        KisNodeSP child = image->root()->at(i);
        if (child->name() == PdfStripBuilder::inkGroupName(index) && child->childCount() > 0) {
            layer = child->at(0);
            break;
        }
    }

    if (layer && m_view && m_view->viewManager() && m_view->viewManager()->nodeManager()) {
        m_view->viewManager()->nodeManager()->slotNonUiActivatedNode(layer);
    }

    m_stripActiveSlot = slot;
    m_index = index;

    say(QStringLiteral("page %1 is now the active slot of the strip").arg(index + 1));
    Q_EMIT pageChanged(m_index, pageCount(), QFileInfo(m_manifest.sourceFile).completeBaseName());
    ensureThumbnail(index - 1);
    ensureThumbnail(index + 1);
    return true;
}

void PdfPageNavigator::lockSlotsBut(int activeSlot, int oldActiveSlot)
{
    Q_UNUSED(oldActiveSlot);

    if (!m_document || !m_document->image()) {
        return;
    }

    KisImageSP image = m_document->image();
    for (int slot = 0; slot < m_stripPages.size(); ++slot) {
        const int page = m_stripPages.at(slot);
        if (page < 0) {
            continue;
        }

        const bool active = (slot == activeSlot);

        for (quint32 i = 0; i < image->root()->childCount(); ++i) {
            KisNodeSP child = image->root()->at(i);
            if (child->name() != PdfStripBuilder::inkGroupName(page)) {
                continue;
            }

            child->setUserLocked(!active);
            for (quint32 c = 0; c < child->childCount(); ++c) {
                child->at(c)->setUserLocked(!active);
            }
        }
    }
}

int PdfPageNavigator::scope() const
{
    return m_scope;
}

void PdfPageNavigator::setScope(int scope)
{
    m_scope = qMax(1, scope);
}

bool PdfPageNavigator::showPage(int index, QString *why)
{
    if (!hasNotebook()) {
        fail(why, QStringLiteral("no notebook is open"));
        return false;
    }
    if (index < 0 || index >= m_manifest.pages.size()) {
        fail(why, QStringLiteral("page %1 is outside the notebook").arg(index + 1));
        return false;
    }

    /// A page that is already inside the open strip costs nothing to reach: no renderer, no
    /// document, no view. Unlocking its slot and asking for it is the whole point of design B --
    /// the 650 ms a page turn costs is almost all document and view, measured on the tablet.
    if (m_document && m_stripPages.contains(index)) {
        return activateWithinStrip(index, why);
    }

    /// Anything else leaves the page that is open, and that page is written before it goes.
    if (m_document && m_document->image() && m_index != index) {
        QString saveError;
        if (!saveCurrentPage(&saveError)) {
            say(QStringLiteral("could not save the page being left: %1").arg(saveError));
        }
    }

    return m_scope > 1 ? buildForStrip(index, why) : buildForSinglePage(index, why);
}

bool PdfPageNavigator::buildForSinglePage(int index, QString *why)
{
    QScopedPointer<PdfRenderBackend> backend(PdfRenderBackend::create());
    if (!backend) {
        fail(why, QStringLiteral("no PDF render backend on this platform"));
        return false;
    }

    const QString source = PdfSession::sourcePath(m_projectDir, m_manifest.sourceFile);
    if (!backend->open(source)) {
        fail(why, QStringLiteral("the renderer cannot open %1").arg(source));
        return false;
    }

    /// Step by step on purpose. Opening a document on Android crashed inside Qt without saying
    /// where, and these lines are what turned "somewhere after the copy" into a stage.
    say(QStringLiteral("rendering page %1").arg(index + 1));

    KisImageSP image = PdfProjectBuilder::buildPageImage(m_manifest.pages.at(index), *backend, 200.0, why);
    if (!image) {
        return false;
    }
    say(QStringLiteral("rendered %1x%2 at %3 dpi").arg(image->width()).arg(image->height()).arg(image->xRes()));

    /// Whatever was already drawn on this page is put back before it is shown. Saving alone is not
    /// enough: a page rebuilt from the source comes back with an untouched Ink layer, so without
    /// this step ink that was written is invisible the moment the page is left and returned to.
    const QString kraPath = QDir(m_projectDir).filePath(m_manifest.pages.at(index).kraFile);
    const QImage savedInk = PdfInkLoader::loadInk(kraPath, nullptr);
    if (!savedInk.isNull()) {
        if (KisPaintLayer *stroke = qobject_cast<KisPaintLayer *>(PdfProjectBuilder::inkStrokeLayer(image).data())) {
            stroke->paintDevice()->convertFromQImage(savedInk, 0, 0, 0);
            say(QStringLiteral("restored %1x%2 of ink from %3")
                    .arg(savedInk.width()).arg(savedInk.height()).arg(kraPath));
        }
    }

    return showImage(image, PdfProjectBuilder::inkStrokeLayer(image), index, PdfStripLayout(), why);
}

bool PdfPageNavigator::showImage(KisImageSP image, KisNodeSP activeNode, int index,
                                 const PdfStripLayout &layout, QString *why)
{
    Q_UNUSED(why);

    KisDocument *document = KisPart::instance()->createDocument();
    document->documentInfo()->setAboutInfo(QStringLiteral("title"),
                                           QFileInfo(m_manifest.sourceFile).completeBaseName());
    document->setCurrentImage(image, true, activeNode);
    document->setProperty("pdfioProjectDir", m_projectDir);
    document->setProperty("pdfioPageIndex", m_manifest.pages.at(index).index);
    say(QStringLiteral("document created, image attached"));

    KisPart::instance()->addDocument(document);
    say(QStringLiteral("document registered"));

    KisMainWindow *window = KisPart::instance()->currentMainwindow();
    say(QStringLiteral("main window %1").arg(window ? "found" : "MISSING"));
    KisView *view = window ? window->addViewAndNotifyLoadingCompleted(document) : nullptr;
    say(QStringLiteral("view %1").arg(view ? "created" : "NOT created"));

    /// The neighbouring pages are shown by the view that has just been created, not by whichever
    /// one this code happens to be running in.
    if (view && view->canvasBase()
        && !view->canvasBase()->decoration(QStringLiteral("pdfioPageStrip"))) {
        view->canvasBase()->addDecoration(
            KisCanvasDecorationSP(new PdfPageStripDecoration(QStringLiteral("pdfioPageStrip"), view)));
    }

    /// Only now, with the new page up, is the old one given back. Closing first would take the
    /// view that is running this very code with it.
    const QPointer<KisDocument> previousDocument = m_document;
    const QPointer<KisView> previousView = m_view;

    m_document = document;
    m_view = view;
    m_index = index;
    m_stripPages.clear();
    m_stripRects.clear();
    for (const PdfStripLayout::Slot &slot : layout.slots()) {
        m_stripPages.append(slot.page);
        m_stripRects.append(slot.rect);
    }
    m_stripActiveSlot = layout.isValid() ? layout.activeSlot() : -1;

    /// Closed on the next turn of the event loop rather than right here. Closing a view and
    /// removing its document re-enters the window layout, and doing that inside the call that is
    /// opening the next page hung: the probe stopped after the first turn and had to be killed.
    if (previousView || previousDocument) {
        const QPointer<KisView> doomedView = previousView;
        const QPointer<KisDocument> doomedDocument = previousDocument;

        QTimer::singleShot(0, this, [doomedView, doomedDocument]() {
            if (doomedView) {
                /// The view takes the document with it: Krita closes a document when its last view
                /// goes. Removing the document as well was a double teardown, and it crashed --
                /// the document died while the view lived on, and a queued signal compressor then
                /// called slotUpdateDocumentTitle on that view, which reached KisDocument::path
                /// through a null document.
                doomedView->closeView();
            } else if (doomedDocument) {
                /// No view was ever made for it, so nobody else will close it.
                KisPart::instance()->removeDocument(doomedDocument, true);
            }
        });
    }

    /// The pages either side are drawn by the strip decoration, and it can only draw what has been
    /// rendered. Asking here means the neighbours fill in as the page is opened, rather than only
    /// once the page selector has been scrolled.
    ensureThumbnail(index - 1);
    ensureThumbnail(index + 1);

    say(QStringLiteral("page %1 of %2 open").arg(index + 1).arg(m_manifest.pages.size()));
    Q_EMIT pageChanged(m_index, pageCount(), QFileInfo(m_manifest.sourceFile).completeBaseName());
    return true;
}

bool PdfPageNavigator::saveCurrentPage(QString *why)
{
    if (!m_document || !m_document->image() || m_index < 0 || m_index >= m_manifest.pages.size()) {
        /// Nothing open is not a failure; it only means there is nothing to write.
        return true;
    }

    /// What belongs to the page that is open. In a strip, that is the page's own group cropped to
    /// its own rectangle; in a document that holds one page it is everything, and no cropping is
    /// needed. Without this a save would write the whole strip as one page's ink.
    QRect pageArea;
    QList<KisNodeSP> inkLayers;
    if (!m_stripPages.isEmpty() && m_stripActiveSlot >= 0
        && m_stripActiveSlot < m_stripRects.size()) {
        pageArea = m_stripRects.at(m_stripActiveSlot);

        for (quint32 i = 0; i < m_document->image()->root()->childCount(); ++i) {
            KisNodeSP child = m_document->image()->root()->at(i);
            if (child->name() != PdfStripBuilder::inkGroupName(m_index)) {
                continue;
            }
            for (quint32 c = 0; c < child->childCount(); ++c) {
                inkLayers.append(child->at(c));
            }
            break;
        }
    }

    const QRect thumbArea = pageArea.isValid()
        ? pageArea
        : QRect(0, 0, m_document->image()->width(), m_document->image()->height());

    /// A thumbnail of the page as it looks, ink included, so the docker can show what each page
    /// holds without opening it. A thumbnail is a scaled copy, not a document, which is the whole
    /// reason this is affordable and rendering neighbouring pages is not.
    const QDir project(m_projectDir);
    const QString thumbPath = project.filePath(m_manifest.pages.at(m_index).thumbFile);
    if (KisPaintDeviceSP projection = m_document->image()->projection()) {
        /// Of the page's own rectangle, or a strip's thumbnail would be a picture of the strip.
        const QImage thumb =
            projection->createThumbnailUncached(ThumbnailPixels, ThumbnailPixels, thumbArea);
        if (!thumb.isNull()) {
            QDir().mkpath(QFileInfo(thumbPath).absolutePath());
            thumb.save(thumbPath, "PNG");
        }
    }

    /// The copy is made while the page is still alive, and it owns its own pixels, so the editing
    /// document can be closed immediately afterwards.
    KisDocument *inkOnly = nullptr;
    if (pageArea.isValid() && !inkLayers.isEmpty()) {
        inkOnly = PdfPageSaver::createInkOnlyDocument(m_document->image(), pageArea, inkLayers, why);
    } else {
        inkOnly = PdfPageSaver::createInkOnlyDocument(m_document->image(), why);
    }
    if (!inkOnly) {
        return false;
    }

    const QString path = QDir(m_projectDir).filePath(
        PdfSession::pageFileName(m_manifest.pages.at(m_index).index));
    QDir().mkpath(QFileInfo(path).absolutePath());

    /// Deleted when the save reports back rather than by waiting: a nested event loop around
    /// sigSavingFinished wedged on the second save.
    QObject::connect(inkOnly, &KisDocument::sigSavingFinished, inkOnly, [inkOnly, path](const QString &) {
        say(QStringLiteral("saved %1 (%2 bytes)").arg(path).arg(QFileInfo(path).size()));
        KisPart::instance()->removeDocument(inkOnly, true);
    });

    if (!PdfPageSaver::saveInkOnly(inkOnly, path, why)) {
        KisPart::instance()->removeDocument(inkOnly, true);
        return false;
    }

    /// The thumbnail of this page has just been rewritten from the ink that was saved, so anything
    /// showing it -- the page selector -- is told, rather than waiting for the page to be opened
    /// again before it notices.
    Q_EMIT thumbnailReady(m_index);

    return true;
}

bool PdfPageNavigator::next(QString *why)
{
    if (m_index + 1 >= pageCount()) {
        fail(why, QStringLiteral("this is the last page"));
        return false;
    }
    return showPage(m_index + 1, why);
}

bool PdfPageNavigator::previous(QString *why)
{
    if (m_index <= 0) {
        fail(why, QStringLiteral("this is the first page"));
        return false;
    }
    return showPage(m_index - 1, why);
}
