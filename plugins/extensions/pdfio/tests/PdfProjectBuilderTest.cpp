/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "backends/poppler/PopplerRenderBackend.h"
#include "session/PdfProjectBuilder.h"
#include "session/PdfSession.h"

#include <kis_group_layer.h>
#include <kis_image.h>
#include <kis_paint_layer.h>

#include <QtTest>

/**
 * The layer contract is what makes the rest of the session safe: if the page ever stops being
 * locked, or the notes stop landing in a group of their own, the ink-only save that follows
 * would quietly write the original artwork into the project.
 */
class PdfProjectBuilderTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testPageImageStructure();
    void testRotatedPageUsesItsDisplayedSize();
    void testFailsWithoutGeometry();

private:
    QString fixturePath(const QString &name) const
    {
        return QStringLiteral(FILES_DATA_DIR) + name;
    }

    PdfPageRecord recordFor(int index) const
    {
        PopplerRenderBackend backend;
        backend.open(fixturePath(QStringLiteral("text-fixture.pdf")));
        return PdfPageRecord{index, backend.pageInfo(index).sizePt,
                             backend.pageInfo(index).rotation,
                             PdfSession::pageFileName(index), QString(), 0};
    }
};

void PdfProjectBuilderTest::testPageImageStructure()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath(QStringLiteral("text-fixture.pdf"))));

    QString why;
    const KisImageSP image =
        PdfProjectBuilder::buildPageImage(recordFor(0), backend, 72.0, &why);
    QVERIFY2(image, qPrintable(why));

    QCOMPARE(image->xRes(), 72.0);
    QCOMPARE(image->yRes(), 72.0);

    KisNodeSP root = image->root();
    QCOMPARE(root->childCount(), 2u);

    /// Bottom layer: the locked page artwork.
    KisNodeSP bottom = root->at(0);
    QVERIFY(bottom);
    QCOMPARE(bottom->name(), PdfProjectBuilder::backgroundLayerName());
    QVERIFY(bottom->userLocked());
    QVERIFY(qobject_cast<KisPaintLayer *>(bottom.data()));

    /// Top layer: the empty group the notes go into, and only that one is writable.
    KisNodeSP top = root->at(1);
    QVERIFY(top);
    QCOMPARE(top->name(), PdfProjectBuilder::inkLayerName());
    QVERIFY(!top->userLocked());
    QVERIFY(qobject_cast<KisGroupLayer *>(top.data()));
    QCOMPARE(top->childCount(), 0u);

    /// The rendered page really landed: the fixture draws text, so something is not white.
    QImage flattened = bottom->paintDevice()->convertToQImage(0, image->bounds());
    QVERIFY(!flattened.isNull());

    bool foundInk = false;
    for (int y = 0; y < flattened.height() && !foundInk; ++y) {
        for (int x = 0; x < flattened.width(); ++x) {
            if (qGray(flattened.pixel(x, y)) < 200) {
                foundInk = true;
                break;
            }
        }
    }
    QVERIFY2(foundInk, "the background layer is blank, the page raster did not land in it");
}

void PdfProjectBuilderTest::testRotatedPageUsesItsDisplayedSize()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath(QStringLiteral("text-fixture.pdf"))));

    const PdfPageRecord rotated = recordFor(1);
    QCOMPARE(rotated.rotation, 90);

    const KisImageSP image = PdfProjectBuilder::buildPageImage(rotated, backend, 72.0);
    QVERIFY(image);

    /// 420 x 595 points at 72 dpi, i.e. the displayed (rotated) page, within a pixel.
    QCOMPARE(image->width(), 420);
    QCOMPARE(image->height(), 595);
}

void PdfProjectBuilderTest::testFailsWithoutGeometry()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath(QStringLiteral("text-fixture.pdf"))));

    QString why;
    PdfPageRecord broken;
    broken.index = 0;
    const KisImageSP image = PdfProjectBuilder::buildPageImage(broken, backend, 72.0, &why);
    QVERIFY(!image);
    QVERIFY(!why.isEmpty());
}

QTEST_MAIN(PdfProjectBuilderTest)
#include "PdfProjectBuilderTest.moc"
