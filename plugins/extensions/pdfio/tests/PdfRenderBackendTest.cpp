/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "backend/PdfRenderBackend.h"
#include "backends/poppler/PopplerRenderBackend.h"

#include <QtTest>

/**
 * Pins the contract the session relies on, using the three page fixture whose geometry was
 * measured with pdfprobe/: a plain page, a page rotated ninety degrees, and a page whose
 * MediaBox does not start at the origin.
 *
 * These are the numbers the whole page -> layer transform is built on, so they belong in a
 * test rather than in a comment.
 */
class PdfRenderBackendTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testOpenAndPageCount();
    void testGeometry_data();
    void testGeometry();
    void testRenderSize_data();
    void testRenderSize();
    void testRenderIsNotBlank();

private:
    QString fixturePath(const QString &name) const
    {
        return QStringLiteral(FILES_DATA_DIR) + name;
    }
};

void PdfRenderBackendTest::testOpenAndPageCount()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath("text-fixture.pdf")));
    QVERIFY(backend.isOpen());
    QCOMPARE(backend.pageCount(), 3);

    PopplerRenderBackend missing;
    QVERIFY(!missing.open(fixturePath("does-not-exist.pdf")));
    QVERIFY(!missing.isOpen());
    QCOMPARE(missing.pageCount(), 0);
}

void PdfRenderBackendTest::testGeometry_data()
{
    QTest::addColumn<int>("page");
    QTest::addColumn<QSizeF>("sizePt");
    QTest::addColumn<int>("rotation");

    /// /Rotate is already applied by the backend, so page 2 reports the rotated size.
    QTest::newRow("plain A4")          << 0 << QSizeF(595, 842) << 0;
    QTest::newRow("rotated 90")        << 1 << QSizeF(420, 595) << 90;
    /// MediaBox [50 30 350 330]: the origin does not leak into the size.
    QTest::newRow("offset media box")  << 2 << QSizeF(300, 300) << 0;
}

void PdfRenderBackendTest::testGeometry()
{
    QFETCH(int, page);
    QFETCH(QSizeF, sizePt);
    QFETCH(int, rotation);

    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath("text-fixture.pdf")));

    const PdfPageInfo info = backend.pageInfo(page);
    QVERIFY(info.isValid());
    QCOMPARE(info.index, page);
    QCOMPARE(info.sizePt, sizePt);
    QCOMPARE(info.rotation, rotation);
}

void PdfRenderBackendTest::testRenderSize_data()
{
    QTest::addColumn<int>("page");
    QTest::addColumn<QSize>("sizePt");
    QTest::addColumn<double>("dpi");

    QTest::newRow("plain A4 at 200 dpi")   << 0 << QSize(595, 842) << 200.0;
    QTest::newRow("rotated at 200 dpi")    << 1 << QSize(420, 595) << 200.0;
    QTest::newRow("offset at 72 dpi")      << 2 << QSize(300, 300) << 72.0;
}

void PdfRenderBackendTest::testRenderSize()
{
    QFETCH(int, page);
    QFETCH(QSize, sizePt);
    QFETCH(double, dpi);

    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath("text-fixture.pdf")));

    const QImage image = backend.renderPage(page, dpi);
    QVERIFY(!image.isNull());

    /// Do not derive the raster size from pt / 72 * dpi: renderers round differently
    /// (pdftoppm gave 834 px where Poppler Qt gave 833 for the same page), which is exactly
    /// why the contract says to read the size of the returned image.
    const double expectedW = sizePt.width() * dpi / 72.0;
    const double expectedH = sizePt.height() * dpi / 72.0;
    QVERIFY2(qAbs(image.width() - expectedW) <= 1.5,
             qPrintable(QStringLiteral("width %1, expected about %2")
                            .arg(image.width()).arg(expectedW)));
    QVERIFY2(qAbs(image.height() - expectedH) <= 1.5,
             qPrintable(QStringLiteral("height %1, expected about %2")
                            .arg(image.height()).arg(expectedH)));
}

void PdfRenderBackendTest::testRenderIsNotBlank()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath("text-fixture.pdf")));

    /// The fixture draws real text, so a rendered page has to contain something dark.
    const QImage image = backend.renderPage(0, 72.0);
    QVERIFY(!image.isNull());

    bool foundInk = false;
    for (int y = 0; y < image.height() && !foundInk; ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qGray(image.pixel(x, y)) < 200) {
                foundInk = true;
                break;
            }
        }
    }
    QVERIFY2(foundInk, "the rendered page is blank, the fixture text did not draw");
}

QTEST_MAIN(PdfRenderBackendTest)
#include "PdfRenderBackendTest.moc"
