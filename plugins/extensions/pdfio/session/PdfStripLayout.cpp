/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfStripLayout.h"

#include <QtMath>

namespace {

/// The blank space between two slots, in image pixels. Part of the cell, so it does not change the
/// image size when the window rolls.
constexpr int SlotGap = 48;

QSize pageSizeInPixels(const PdfSessionManifest &manifest, const PdfPageRecord &page, qreal dpi)
{
    Q_UNUSED(manifest);
    if (!page.sizePt.isValid()) {
        return QSize();
    }
    return QSize(qCeil(page.sizePt.width() * dpi / 72.0),
                 qCeil(page.sizePt.height() * dpi / 72.0));
}

} // namespace

PdfStripLayout PdfStripLayout::forWindow(const PdfSessionManifest &manifest,
                                         int activePage,
                                         int scope,
                                         qreal dpi)
{
    PdfStripLayout layout;

    if (!manifest.isValid() || manifest.pages.isEmpty() || dpi <= 0) {
        return layout;
    }
    if (activePage < 0 || activePage >= manifest.pages.size()) {
        return layout;
    }

    /// Odd, and at least one.
    scope = qMax(1, scope);
    if (scope % 2 == 0) {
        ++scope;
    }
    scope = qMin(scope, manifest.pages.size());

    /// Every slot is the same cell, sized to the largest page, so that the image size does not
    /// change as the window moves. Wasteful for a notebook of mixed sizes, and the price of not
    /// rebuilding the document on every turn.
    QSize cell;
    for (const PdfPageRecord &page : manifest.pages) {
        const QSize size = pageSizeInPixels(manifest, page, dpi);
        cell.setWidth(qMax(cell.width(), size.width()));
        cell.setHeight(qMax(cell.height(), size.height()));
    }
    if (cell.isEmpty()) {
        return layout;
    }

    const int half = scope / 2;

    /// The window is centred on the active page as far as the ends of the notebook allow.
    int first = activePage - half;
    first = qBound(0, first, qMax(0, manifest.pages.size() - scope));

    for (int i = 0; i < scope; ++i) {
        const int pageIndex = first + i;

        Slot slot;
        slot.page = pageIndex < manifest.pages.size() ? pageIndex : -1;

        const QSize pageSize = slot.page >= 0
            ? pageSizeInPixels(manifest, manifest.pages.at(slot.page), dpi)
            : QSize();

        /// Centred horizontally inside the cell, at the top of it.
        const int x = (cell.width() - pageSize.width()) / 2;
        slot.rect = QRect(x, i * (cell.height() + SlotGap), pageSize.width(), pageSize.height());

        if (slot.page == activePage) {
            layout.m_activeSlot = i;
        }

        layout.m_slots.append(slot);
    }

    layout.m_activePage = activePage;
    layout.m_imageSize = QSize(cell.width(), scope * (cell.height() + SlotGap));
    return layout;
}

bool PdfStripLayout::isValid() const
{
    return !m_slots.isEmpty() && m_imageSize.isValid() && m_activeSlot >= 0;
}

QSize PdfStripLayout::imageSize() const
{
    return m_imageSize;
}

QList<PdfStripLayout::Slot> PdfStripLayout::slots() const
{
    return m_slots;
}

int PdfStripLayout::activePage() const
{
    return m_activePage;
}

int PdfStripLayout::activeSlot() const
{
    return m_activeSlot;
}

int PdfStripLayout::slotForPage(int page) const
{
    for (int i = 0; i < m_slots.size(); ++i) {
        if (m_slots.at(i).page == page) {
            return i;
        }
    }
    return -1;
}

int PdfStripLayout::pageAt(const QPoint &point) const
{
    for (const Slot &slot : m_slots) {
        if (slot.page >= 0 && slot.rect.contains(point)) {
            return slot.page;
        }
    }
    return -1;
}
