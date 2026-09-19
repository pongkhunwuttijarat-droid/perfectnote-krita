/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "session/PdfStripLayout.h"

#include <QtTest>

/**
 * The geometry design B rests on. The load bearing property is that the image size does not change
 * as the window moves: rolling the window repaints one slot rather than rebuilding the document,
 * and that only works if the image stays the same size.
 */
class PdfStripLayoutTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testWindowIsCentred();
    void testWindowClampsAtTheEnds();
    void testImageSizeDoesNotDependOnTheActivePage();
    void testSlotsHoldWholePages();
    void testPageAtDistinguishesGapsFromPages();
    void testEvenScopeIsMadeOdd();
    void testScopeLargerThanTheNotebook();
    void testRefusesNonsense();

private:
    /// Three pages: A4, then A5 rotated, then a small square. Mixed on purpose.
    PdfSessionManifest manifest() const
    {
        PdfSessionManifest manifest;
        manifest.sourceFile = QStringLiteral("fixture.pdf");
        manifest.sourceSha256 = QByteArrayLiteral("0000");
        manifest.sourceByteSize = 1;

        const QSizeF sizes[] = { QSizeF(595, 842), QSizeF(420, 595), QSizeF(300, 300) };
        for (int i = 0; i < 3; ++i) {
            PdfPageRecord page;
            page.index = i;
            page.sizePt = sizes[i];
            page.rotation = i == 1 ? 90 : 0;
            page.kraFile = QStringLiteral("pages/p%1.kra").arg(i + 1, 4, 10, QLatin1Char('0'));
            page.thumbFile = QStringLiteral("thumbs/p%1.png").arg(i + 1, 4, 10, QLatin1Char('0'));
            manifest.pages.append(page);
        }
        return manifest;
    }
};

void PdfStripLayoutTest::testWindowIsCentred()
{
    const PdfStripLayout layout = PdfStripLayout::forWindow(manifest(), 1, 3, 200.0);

    QVERIFY(layout.isValid());
    QCOMPARE(layout.activePage(), 1);
    QCOMPARE(layout.activeSlot(), 1);

    const QList<PdfStripLayout::Slot> slots = layout.slots();
    QCOMPARE(slots.size(), 3);
    QCOMPARE(slots.at(0).page, 0);
    QCOMPARE(slots.at(1).page, 1);
    QCOMPARE(slots.at(2).page, 2);
}

void PdfStripLayoutTest::testWindowClampsAtTheEnds()
{
    /// With three pages and a scope of three there is only one possible window.
    const PdfStripLayout first = PdfStripLayout::forWindow(manifest(), 0, 3, 200.0);
    QCOMPARE(first.slots().at(first.activeSlot()).page, 0);

    const PdfStripLayout last = PdfStripLayout::forWindow(manifest(), 2, 3, 200.0);
    QCOMPARE(last.slots().at(last.activeSlot()).page, 2);
}

void PdfStripLayoutTest::testImageSizeDoesNotDependOnTheActivePage()
{
    const PdfStripLayout a = PdfStripLayout::forWindow(manifest(), 0, 3, 200.0);
    const PdfStripLayout b = PdfStripLayout::forWindow(manifest(), 1, 3, 200.0);
    const PdfStripLayout c = PdfStripLayout::forWindow(manifest(), 2, 3, 200.0);

    QVERIFY(!a.imageSize().isEmpty());
    QCOMPARE(a.imageSize(), b.imageSize());
    QCOMPARE(b.imageSize(), c.imageSize());
}

void PdfStripLayoutTest::testSlotsHoldWholePages()
{
    const PdfStripLayout layout = PdfStripLayout::forWindow(manifest(), 1, 3, 200.0);
    const QList<PdfStripLayout::Slot> slots = layout.slots();

    for (const PdfStripLayout::Slot &slot : slots) {
        QVERIFY(slot.page >= 0);

        /// The slot's rect is the page at its own size, inside the strip.
        const PdfPageRecord &page = manifest().pages.at(slot.page);
        QCOMPARE(slot.rect.width(), qRound(page.sizePt.width() * 200.0 / 72.0));
        QCOMPARE(slot.rect.height(), qRound(page.sizePt.height() * 200.0 / 72.0));

        /// And it is inside the image.
        QVERIFY(QRect(QPoint(0, 0), layout.imageSize()).contains(slot.rect));
    }

    /// The slots do not overlap each other.
    for (int i = 1; i < slots.size(); ++i) {
        QVERIFY(slots.at(i).rect.top() > slots.at(i - 1).rect.bottom());
    }
}

void PdfStripLayoutTest::testPageAtDistinguishesGapsFromPages()
{
    const PdfStripLayout layout = PdfStripLayout::forWindow(manifest(), 1, 3, 200.0);
    const QList<PdfStripLayout::Slot> slots = layout.slots();

    QCOMPARE(layout.pageAt(slots.at(0).rect.center()), 0);
    QCOMPARE(layout.pageAt(slots.at(1).rect.center()), 1);
    QCOMPARE(layout.pageAt(slots.at(2).rect.center()), 2);

    /// Below the last page, in the gap between slots, there is no page.
    const QPoint inGap(slots.at(0).rect.center().x(), slots.at(0).rect.bottom() + 4);
    QCOMPARE(layout.pageAt(inGap), -1);
}

void PdfStripLayoutTest::testEvenScopeIsMadeOdd()
{
    /// An even scope would put the active page off centre, so it is rounded up.
    const PdfStripLayout layout = PdfStripLayout::forWindow(manifest(), 3 - 1, 2, 200.0);
    QVERIFY(layout.isValid());
    QCOMPARE(layout.slots().size(), 3);
}

void PdfStripLayoutTest::testScopeLargerThanTheNotebook()
{
    const PdfStripLayout layout = PdfStripLayout::forWindow(manifest(), 1, 9, 200.0);
    QVERIFY(layout.isValid());
    QCOMPARE(layout.slots().size(), 3);
    QCOMPARE(layout.activeSlot(), 1);
}

void PdfStripLayoutTest::testRefusesNonsense()
{
    QVERIFY(!PdfStripLayout::forWindow(PdfSessionManifest(), 0, 3, 200.0).isValid());
    QVERIFY(!PdfStripLayout::forWindow(manifest(), 9, 3, 200.0).isValid());
    QVERIFY(!PdfStripLayout::forWindow(manifest(), 0, 3, 0.0).isValid());
}

QTEST_MAIN(PdfStripLayoutTest)
#include "PdfStripLayoutTest.moc"
