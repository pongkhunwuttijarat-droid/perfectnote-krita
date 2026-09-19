/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFPAGENAVIGATOR_H
#define PDFPAGENAVIGATOR_H

/// Included rather than forward declared: the navigator holds a QScopedPointer to it, and that
/// needs the complete type for its destructor.
#include "backend/PdfRenderBackend.h"
#include "session/PdfSessionManifest.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

class KisDocument;
class KisView;

/**
 * Which page of which notebook is open, held once per process.
 *
 * Not per plugin instance: a Krita view plugin is created per view, and paging closes the view
 * of the page being left behind, which would take the instance that is doing the paging with it.
 *
 * A page costs about 41 MB resident, measured across twelve pages, so the notebook keeps a
 * bounded set: opening a page closes the one before it. The page artwork is not lost by that,
 * because it is rendered again from the bundled source.
 */
class PdfPageNavigator : public QObject
{
    Q_OBJECT
public:
    static PdfPageNavigator *instance();

    /// Wraps \a pdfPath into a project if needed and shows its first page.
    bool openNotebook(const QString &pdfPath, QString *why = nullptr);

    bool showPage(int index, QString *why = nullptr);
    bool next(QString *why = nullptr);
    bool previous(QString *why = nullptr);

    bool hasNotebook() const;

    /// The document of the page that is open, if one is.
    KisDocument *currentDocument() const;

    /// The view showing it, for code that needs the canvas rather than the document.
    KisView *currentView() const;

    /**
     * Makes sure the page has a thumbnail, rendering one at thumbnail resolution when it has none.
     *
     * Queueing is cheap and the work happens one page at a time, so a page selector can ask for
     * every page it shows without the window stalling. Pages that were drawn on already have a
     * thumbnail, taken from their own projection; this is for the ones that were never opened.
     */
    void ensureThumbnail(int index);

    /**
     * Whether panning past the edge of a page turns to the next one.
     *
     * Off is a reasonable choice: the gesture that turns a page is the same one used to look at
     * the bottom of a page, and someone who reads that way will not want the notebook moving
     * under them.
     */
    bool scrollFollowEnabled() const;
    void setScrollFollowEnabled(bool enabled);

    /// What the export needs to walk the notebook and find its source.
    const PdfSessionManifest &manifest() const;
    QString sourcePath() const;
    int pageCount() const;
    int currentIndex() const;
    QString projectDir() const;

Q_SIGNALS:
    /// Emitted whenever the open page or the notebook itself changes, so a navigator widget can
    /// follow along without polling.
    void pageChanged(int index, int pageCount, const QString &label);

    /// A thumbnail that was missing has been written, so a view showing that page can update.
    void thumbnailReady(int index);

private:
    PdfPageNavigator() = default;

    /// Closes the page that is open, freeing its document and its view.
    void closeCurrentPage();

    /**
     * Writes the ink of the page that is open, if one is.
     *
     * Called before a page is left behind, not only when the user asks: turning a page used to
     * remove the document, and nothing had ever been saved from it, so the ink went with it.
     */
    bool saveCurrentPage(QString *why = nullptr);

    static QString projectRoot();

    /// Acts on where the middle of the view has settled.
    void checkScrollFollow();

    /**
     * Which page the given document point falls on: the open one, a neighbour above or below it,
     * or -1 for none. The layout is the one the strip decoration draws, so the two agree about
     * where the neighbouring pages are.
     */
    int pageAtDocumentPoint(const QPointF &point, qreal zoom) const;

    QString m_projectDir;
    PdfSessionManifest m_manifest;
    QPointer<KisDocument> m_document;
    QPointer<KisView> m_view;
    int m_index = -1;

    bool m_scrollFollow = true;
    QTimer *m_scrollWatch = nullptr;

    void makeOneThumbnail();

    QList<int> m_thumbnailQueue;
    QTimer *m_thumbnailTimer = nullptr;

    /// Kept open between thumbnails: parsing the source once is worth more than the thumbnails.
    QScopedPointer<PdfRenderBackend> m_thumbnailBackend;

    /// So one continued gesture does not turn several pages.
    qint64 m_lastTurn = 0;

    /// The page the middle of the view is over, and since when, so a page is only turned to once
    /// the view has stopped there.
    int m_candidatePage = -1;
    qint64 m_candidateSince = 0;


};

#endif // PDFPAGENAVIGATOR_H
