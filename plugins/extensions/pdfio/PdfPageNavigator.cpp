/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfPageNavigator.h"

#include <cstdio>

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include "backend/PdfRenderBackend.h"
#include "session/PdfPageSaver.h"
#include "session/PdfProjectBuilder.h"
#include "session/PdfSession.h"

#include <KoDocumentInfo.h>

#include <KisDocument.h>
#include <KisMainWindow.h>
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
    return shown;
}

void PdfPageNavigator::closeCurrentPage()
{
    /// The view first: a view outlives its document otherwise, and the point of this is to give
    /// the memory back.
    if (m_view) {
        m_view->closeView();
        m_view = nullptr;
    }
    if (m_document) {
        KisPart::instance()->removeDocument(m_document, true);
        m_document = nullptr;
    }
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

    KisDocument *document = KisPart::instance()->createDocument();
    document->documentInfo()->setAboutInfo(QStringLiteral("title"),
                                           QFileInfo(m_manifest.sourceFile).completeBaseName());
    document->setCurrentImage(image, true, PdfProjectBuilder::inkStrokeLayer(image));
    document->setProperty("pdfioProjectDir", m_projectDir);
    document->setProperty("pdfioPageIndex", m_manifest.pages.at(index).index);
    say(QStringLiteral("document created, image attached"));

    KisPart::instance()->addDocument(document);
    say(QStringLiteral("document registered"));

    KisMainWindow *window = KisPart::instance()->currentMainwindow();
    say(QStringLiteral("main window %1").arg(window ? "found" : "MISSING"));
    KisView *view = window ? window->addViewAndNotifyLoadingCompleted(document) : nullptr;
    say(QStringLiteral("view %1").arg(view ? "created" : "NOT created"));

    /// Only now, with the new page up, is the old one given back. Closing first would take the
    /// view that is running this very code with it.
    const QPointer<KisDocument> previousDocument = m_document;
    const QPointer<KisView> previousView = m_view;

    /// The page being left is written before it is closed. This used to be missing, and turning a
    /// page threw the ink away: the document was removed and nothing had ever been saved from it.
    if (previousDocument && previousDocument->image()) {
        QString saveError;
        if (!saveCurrentPage(&saveError)) {
            say(QStringLiteral("could not save the page being left: %1").arg(saveError));
        }
    }

    m_document = document;
    m_view = view;
    m_index = index;

    if (previousView) {
        previousView->closeView();
    }
    if (previousDocument) {
        KisPart::instance()->removeDocument(previousDocument, true);
    }

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

    /// The copy is made while the page is still alive, and it owns its own pixels, so the editing
    /// document can be closed immediately afterwards.
    KisDocument *inkOnly = PdfPageSaver::createInkOnlyDocument(m_document->image(), why);
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
