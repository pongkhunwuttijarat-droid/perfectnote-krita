/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "backends/poppler/PopplerRenderBackend.h"
#include "session/PdfStripBuilder.h"

#include <QtTest>

#include <limits>

#include <kis_group_layer.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>

/**
 * The strip, as a layer tree. The assertion that matters is the locking: the promise that the
 * neighbouring pages are shown but not yours to draw on is only true if Krita refuses the stroke,
 * which means both the group and its paint layer have to be locked on every slot but the active
 * one.
 */
class PdfStripBuilderTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testImageIsTheLayoutSize();
    void testEverySlotHasAPageAndAnInkGroup();
    void testOnlyTheActiveSlotIsUnlocked();
    void testPagesAreWhereTheLayoutSays();
    void testPaperIsBelowEveryInkGroup();

private:
    QString fixturePath() const
    {
        return QStringLiteral(FILES_DATA_DIR) + QStringLiteral("text-fixture.pdf");
    }

    PdfSessionManifest manifestFor(PdfRenderBackend &backend) const
    {
        PdfSessionManifest manifest;
        manifest.sourceFile = QStringLiteral("text-fixture.pdf");
        manifest.sourceSha256 = QByteArrayLiteral("0000");
        manifest.sourceByteSize = 1;
        for (int i = 0; i < backend.pageCount(); ++i) {
            const PdfPageInfo info = backend.pageInfo(i);
            manifest.pages.append(PdfPageRecord{i, info.sizePt, info.rotation,
                                                QStringLiteral("pages/p%1.kra")
                                                    .arg(i + 1, 4, 10, QLatin1Char('0')),
                                                QString(), 0});
        }
        return manifest;
    }

    KisNodeSP childNamed(const KisImageSP &image, const QString &name) const
    {
        KisNodeSP root = image->root();
        for (quint32 i = 0; i < root->childCount(); ++i) {
            if (root->at(i)->name() == name) {
                return root->at(i);
            }
        }
        return KisNodeSP();
    }
};

void PdfStripBuilderTest::testImageIsTheLayoutSize()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath()));

    QString why;
    const PdfStripBuilder::Strip strip = PdfStripBuilder::build(manifestFor(backend), 1, 3, 200.0,
                                                               backend, QString(), &why);
    QVERIFY2(strip.image, qPrintable(why));
    QVERIFY(strip.layout.isValid());
    QCOMPARE(QSize(strip.image->width(), strip.image->height()), strip.layout.imageSize());
}

void PdfStripBuilderTest::testEverySlotHasAPageAndAnInkGroup()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath()));

    QString why;
    const PdfStripBuilder::Strip strip = PdfStripBuilder::build(manifestFor(backend), 1, 3, 200.0,
                                                               backend, QString(), &why);
    QVERIFY2(strip.image, qPrintable(why));

    /// Three pages: three backgrounds, three ink groups, and the desk underneath them all.
    QCOMPARE(strip.image->root()->childCount(), 7u);
    QCOMPARE(strip.image->root()->at(0)->name(), QStringLiteral("Desk"));

    for (const PdfStripLayout::Slot &slot : strip.layout.slots()) {
        QVERIFY(slot.page >= 0);

        KisNodeSP background = childNamed(strip.image, PdfStripBuilder::backgroundLayerName(slot.page));
        QVERIFY(background);
        QVERIFY(qobject_cast<KisPaintLayer *>(background.data()));

        KisNodeSP ink = childNamed(strip.image, PdfStripBuilder::inkGroupName(slot.page));
        QVERIFY(ink);
        QVERIFY(qobject_cast<KisGroupLayer *>(ink.data()));
        QCOMPARE(ink->childCount(), 1u);
        QVERIFY(qobject_cast<KisPaintLayer *>(ink->at(0).data()));
    }
}

void PdfStripBuilderTest::testOnlyTheActiveSlotIsUnlocked()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath()));

    QString why;
    const PdfStripBuilder::Strip strip = PdfStripBuilder::build(manifestFor(backend), 1, 3, 200.0,
                                                               backend, QString(), &why);
    QVERIFY2(strip.image, qPrintable(why));

    const int activePage = strip.layout.activePage();
    QVERIFY(activePage >= 0);
    QVERIFY(strip.activeInkLayer);

    for (const PdfStripLayout::Slot &slot : strip.layout.slots()) {
        KisNodeSP background = childNamed(strip.image, PdfStripBuilder::backgroundLayerName(slot.page));
        KisNodeSP ink = childNamed(strip.image, PdfStripBuilder::inkGroupName(slot.page));
        QVERIFY(background);
        QVERIFY(ink);

        /// The page itself is never the user's to edit.
        QVERIFY(background->userLocked());

        if (slot.page == activePage) {
            QVERIFY2(!ink->userLocked(), "the active slot's ink must be paintable");
            QVERIFY2(!ink->at(0)->userLocked(), "the active slot's layer must be paintable");
            QCOMPARE(KisNodeSP(ink->at(0)), strip.activeInkLayer);
        } else {
            QVERIFY2(ink->userLocked(), "a neighbour's ink group must be locked");
            QVERIFY2(ink->at(0)->userLocked(), "a neighbour's layer must be locked");
        }
    }
}

void PdfStripBuilderTest::testPagesAreWhereTheLayoutSays()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath()));

    QString why;
    const PdfStripBuilder::Strip strip = PdfStripBuilder::build(manifestFor(backend), 1, 3, 200.0,
                                                               backend, QString(), &why);
    QVERIFY2(strip.image, qPrintable(why));

    /// The page is painted at its own place in the strip, not at the origin: the top of its own
    /// slot, which is where the ink is expected to line up too.
    for (const PdfStripLayout::Slot &slot : strip.layout.slots()) {
        KisNodeSP background = childNamed(strip.image, PdfStripBuilder::backgroundLayerName(slot.page));
        const QRect bounds = background->paintDevice()->exactBounds();

        QCOMPARE(bounds.width(), slot.rect.width());
        QCOMPARE(bounds.height(), slot.rect.height());
        QCOMPARE(bounds.top(), slot.rect.top());
        QCOMPARE(bounds.left(), slot.rect.left());
    }
}

void PdfStripBuilderTest::testPaperIsBelowEveryInkGroup()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath()));

    QString why;
    const PdfStripBuilder::Strip strip = PdfStripBuilder::build(manifestFor(backend), 1, 3, 200.0,
                                                               backend, QString(), &why);
    QVERIFY2(strip.image, qPrintable(why));

    /// All of the paper first, then all of the ink. Adding a slot at a time puts the next page's
    /// paper above this page's ink, and a stroke that strays outside its own page disappears behind
    /// the page below it -- reported exactly as "the active page did not change", because the
    /// stroke was there and hidden.
    int lowestInk = std::numeric_limits<int>::max();
    int highestPaper = -1;

    KisNodeSP root = strip.image->root();
    for (quint32 i = 0; i < root->childCount(); ++i) {
        const QString name = root->at(i)->name();
        const int index = int(i);

        if (name == QStringLiteral("Desk")) {
            continue;
        }

        const bool isInk = qobject_cast<KisGroupLayer *>(root->at(i).data()) != nullptr;
        if (isInk) {
            lowestInk = qMin(lowestInk, index);
        } else {
            highestPaper = qMax(highestPaper, index);
        }
    }

    QVERIFY(lowestInk != std::numeric_limits<int>::max());
    QVERIFY(highestPaper >= 0);
    QVERIFY2(lowestInk > highestPaper,
             "every ink group has to sit above every page, or ink vanishes behind the next page");
}

QTEST_MAIN(PdfStripBuilderTest)
#include "PdfStripBuilderTest.moc"
