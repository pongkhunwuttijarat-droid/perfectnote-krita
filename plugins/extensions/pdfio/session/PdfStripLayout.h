/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFSTRIPLAYOUT_H
#define PDFSTRIPLAYOUT_H

#include "session/PdfSessionManifest.h"

#include <QList>
#include <QRect>
#include <QSize>

/**
 * Where each page of a strip sits inside the strip image.
 *
 * Pure geometry, separated from everything that builds or paints, because this is the part of
 * design B that has to be right and the part that can be tested without a canvas.
 *
 * The strip is vertical and its image size is fixed for a given scope: every slot is the same
 * cell, sized to the largest page in the notebook, and a page sits at the top of its own cell at
 * its own size. A fixed image size is the point -- rolling the window repaints the slot that goes
 * out of view rather than rebuilding the document, which is what makes a page turn cost a render
 * instead of a document and a view.
 *
 * Pages are laid out in page order, so the strip reads downwards, and the window is centred on
 * the active page as far as the ends of the notebook allow.
 */
class PdfStripLayout
{
public:
    struct Slot {
        /// The page this slot holds, or -1 when the window runs off either end of the notebook.
        int page = -1;
        /// Where the page sits in the strip image, in pixels.
        QRect rect;

        /// The whole band this slot owns, page or no page. Clearing a slot means clearing this,
        /// because the page that was there may have been larger than the one arriving.
        QRect cell;
    };

    PdfStripLayout() = default;

    /**
     * The window of \a scope slots centred on \a activePage.
     *
     * \a scope is forced odd so that the active page has as many pages behind it as ahead, which
     * is what lets it move within the window before the window has to roll.
     */
    static PdfStripLayout forWindow(const PdfSessionManifest &manifest,
                                    int activePage,
                                    int scope,
                                    qreal dpi);

    bool isValid() const;
    QSize imageSize() const;
    QList<Slot> slots() const;
    int activePage() const;
    int activeSlot() const;
    int slotForPage(int page) const;

    /// Which page a point of the strip image falls on, or -1 for a gap or an empty slot.
    int pageAt(const QPoint &point) const;

private:
    QList<Slot> m_slots;
    QSize m_imageSize;
    int m_activeSlot = -1;
    int m_activePage = -1;
};

#endif // PDFSTRIPLAYOUT_H
