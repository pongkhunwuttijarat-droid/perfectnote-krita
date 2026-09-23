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
#include "session/PdfPageWindow.h"
#include "session/PdfSessionManifest.h"
#include "session/PdfStripLayout.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <QRect>

#include <functional>

#include <kis_types.h>

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

    /**
     * Writes every page that is currently in the strip, not only the one that is open.
     *
     * The strip holds its ink in one layer, and which page a stroke belongs to is decided by the
     * rectangle it sits in -- so the cropping can be done whenever, for as many pages as are open,
     * rather than only for the page being left.
     */
    bool saveStripPages();

    /// The view showing it, for code that needs the canvas rather than the document.
    KisView *currentView() const;

    /**
     * How many pages are held at full resolution around the open one.
     *
     * One is design A: a document per page, and a page turn rebuilds it. More is design B: a strip
     * of pages in one document, where turning to a page that is already in the strip is unlocking
     * its slot rather than building anything. See docs/PDFIO-DESIGN-STRIP.md.
     */
    int scope() const;
    void setScope(int scope);

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

    /// Which pages are open, and what the policy has had to do to keep that bounded. Exposed so
    /// the page selector can show the window and so the counters are observable without a debugger.
    const PdfPageWindow &pageWindow() const;
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
    /// Installs the window's save hook and sets its bound to the current scope, once. The page
    /// switch then cannot run without the policy in front of it.
    PdfPageNavigator();

    /// Closes the page that is open, freeing its document and its view.
    void closeCurrentPage();

    /**
     * Writes the ink of the page that is open, if one is.
     *
     * Called before a page is left behind, not only when the user asks: turning a page used to
     * remove the document, and nothing had ever been saved from it, so the ink went with it.
     */
    bool saveCurrentPage(QString *why = nullptr, int index = -1, std::function<void()> then = nullptr);

    /// Writes one page of the strip: its rectangle cropped out of the single ink layer. \a then
    /// is called once the save has finished, which is how the pages are chained.
    bool savePage(int index, std::function<void()> then = nullptr);

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

    /// How many pages the open document holds at full resolution, and at what resolution.
    ///
    /// Three, which is design B. Verified end to end before it was turned on: a mark drawn a
    /// hundred pixels into the page comes back a hundred pixels into an artifact the size of the
    /// page and not of the strip, and turning between pages already in the strip builds nothing.
    /// One was the safe answer while the cropping was unproven, because saving a strip without
    /// cropping writes several pages into one page's ink, quietly.
    /// Three: the page being written on, one above it and one below.
    ///
    /// It was five, to keep a run of turns inside one strip. Five also means the pages two away
    /// stay in the document, and the page a long way back is still there to scroll to, which is not
    /// what a notebook should show -- one above and one below is what was asked for.
    ///
    /// The price is that the window is rebuilt every other turn, because rolling it -- repainting
    /// the one slot that goes out instead of building a new strip -- is not written yet. That is
    /// the piece that will make a run of turns continuous.
    /// One page at a time, which is design A: one document per page, one view per page, the shape
    /// this was built and verified in -- open, draw, turn with an automatic save, export, and the
    /// ink coming back where it was drawn.
    ///
    /// The strip, design B, is behind this number and is not working well enough to ship: pages
    /// above and below in one document, the window rolling rather than rebuilding, the active page
    /// following the middle of the viewport. Raising it to three turns that on. It is left at one
    /// deliberately, so the notebook is usable while the strip is finished.
    int m_scope = 1;
    qreal m_dpi = 200.0;

    /**
     * Which pages may stay open, and the rule that keeps closing one from losing ink.
     *
     * Its bound is \ref m_scope, so design A is a window of one and design B would be a window of
     * three. What it adds over the old "opening a page closes the one before it" is the dirty
     * flag: a page with unsaved ink is never the one that is dropped. When every slot holds such a
     * page, the least recently used one is saved to make room, and a save that fails refuses the
     * page turn instead of discarding the page.
     */
    PdfPageWindow m_window;

    /// The pages the open strip holds and where each sits. Empty when the document is a single
    /// page, which is also how the code tells the two apart.
    QList<int> m_stripPages;
    QList<QRect> m_stripRects;

    /// The paper layer of each slot, in slot order, for repainting one of them.
    QList<KisNodeSP> m_stripPaper;
    int m_stripActiveSlot = -1;

    void makeOneThumbnail();

    /**
     * Opens \a index by building a document for it alone, or for a strip of pages around it.
     *
     * A page that is already in the open strip needs neither: activateWithinStrip unlocks its slot
     * and asks for it, which is the difference between a page turn that costs 650 ms and one that
     * costs nothing. The 650 ms is mostly the document and the view, measured on the tablet, and
     * the strip exists to not pay it.
     */
    bool buildForSinglePage(int index, QString *why);
    bool buildForStrip(int index, QString *why);

    /**
     * Moves the window one page without building anything.
     *
     * The image is the same size for any window, because every slot is the same cell, so reaching
     * a page that is not in the strip only means repainting the slots whose page changed. That is
     * what makes a run of page turns continuous instead of a rebuild every other turn.
     */
    bool rollToPage(int index, QString *why);
    bool activateWithinStrip(int index, QString *why);

    /// Creates the document, its view and the strip decoration, and gives the page that was open
    /// back to the event loop. Shared by both ways of opening one.
    bool showImage(KisImageSP image, KisNodeSP activeNode, int index,
                   const PdfStripLayout &layout, QString *why);

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
