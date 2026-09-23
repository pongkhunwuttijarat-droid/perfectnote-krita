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

    /**
     * Writes what the open page -- every page the window holds -- still has unsaved, so that
     * closing a tab or quitting Krita cannot lose ink.
     *
     * Returns true when the document is safe to close: either it had nothing unsaved, or every
     * page's ink reached its artifact. Returns false when a write could not be completed, and the
     * caller must then NOT close silently -- Krita's own "do you want to save it?" prompt is what
     * should run, so the user is told rather than the ink dropped.
     *
     * Hung off the view (see showImage()) and off the application's aboutToQuit, so both closing
     * our tab and quitting reach it before Krita would ask about the document.
     */
    bool prepareForClose();

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

    /// Saves one page and waits, bounded, for the file to have been written: saveCurrentPage()
    /// returns when the write has started, and a close cannot go on before it has landed.
    bool savePageAndWait(int index, QString *why);

    /// Hangs prepareForClose() on the application's own quit, once.
    void hookApplicationQuitOnce();

    static QString projectRoot();

    /// Acts on where the middle of the view has settled.
    void checkScrollFollow();

    /// Which slot of the open window the given document point is over, or -1. Window-local: it
    /// reads m_stripCells and nothing about the notebook.
    int windowSlotFor(const QPointF &point) const;

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

    /// True while a page turn was started by the view following its centre instead of by the user
    /// asking for the next page. Such a turn must NOT move the view: the canvas is already where
    /// the user put it, and centring it again drags the centre back over the boundary it has just
    /// crossed -- which is exactly what "the active page does not change" looked like.
    bool m_turnFromScroll = false;

    /// Nothing is decided about the centre before this moment. The view is zoomed and centred by
    /// this code when a page opens; a tick that lands inside that has been seen to report a zoom of
    /// 47 while the canvas was at 0.14, and a centre at the document origin.
    qint64 m_viewSettleUntil = 0;

    /// When the last dropped reading was written out, so a canvas that never settles cannot flood.
    qint64 m_lastRejectLog = 0;

    /// When the follow last said anything at all. The switch being off and the follow running
    /// without ever deciding anything used to look exactly the same in the log -- an empty one --
    /// and telling them apart cost an afternoon.
    qint64 m_lastFollowLog = 0;

    /// True while a roll of the strip window is in progress. rollToPage() calls back into
    /// activateWithinStrip(), which is the very place that decides to roll -- without this the two
    /// called each other and the window rolled once a second, repainting every slot it held.
    bool m_rollingWindow = false;

    /// When the window last rolled, so a page sitting on the edge cannot roll it on every tick.
    qint64 m_lastWindowRoll = 0;

    /// True while several pages are being written one after another. The write waits on a nested
    /// event loop, and the follow timer keeps firing inside it: without this a page turn could run
    /// in the middle of the save that is cropping the very layer it would move.
    bool m_savingPages = false;

    /// The document the fit zoom has already been applied to. Fitting again on every page turn is
    /// what made the page under the centre jump around: the zoom changed, the centre moved with it,
    /// and the page the follow computed changed twice in 600 ms (0.25 <-> 0.667 in the log).
    QPointer<KisDocument> m_zoomPlacedFor;

    /// The view's own page system: which slot of the window the middle of the viewport is over.
    /// Window-local geometry (0..slots-1) that knows nothing about notebook page numbers. The two
    /// systems meet in one place -- windowSlotFor() and the single m_stripPages.at(slot) that turns
    /// a slot into a page -- and after a roll this is reset from the active page, so a changed
    /// mapping can never be mistaken for the user having moved.
    int m_windowSlot = -1;
    QTimer *m_scrollWatch = nullptr;

    /// Whether prepareForClose() has been hung off the application's quit already.
    bool m_quitHookInstalled = false;

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

    /// The band each slot owns, in slot order. The cells tile the strip; the page rectangles inside
    /// them do not when the pages are of different sizes, and it is the band that decides which page
    /// the centre of the viewport is over.
    QList<QRect> m_stripCells;

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
    /// a centreOn says which page the new window is centred on; the page that becomes active is
    /// a index either way. Rolling down leaves the active page one slot in from the top rather
    /// than in the middle, so the page just left stays visible above it and the follow does not
    /// turn straight back to it. -1 centres on a index.
    bool rollToPage(int index, QString *why, int centreOn = -1);
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
