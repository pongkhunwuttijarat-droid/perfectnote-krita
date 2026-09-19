/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "backends/poppler/PopplerRenderBackend.h"
#include "session/PdfExporter.h"
#include "session/PdfSession.h"

#include <QtTest>

/**
 * The export contract, checked by rendering the result rather than trusting the bytes: the ink
 * has to land where it was drawn, on a plain page, on a rotated page and on a page whose
 * MediaBox does not start at the origin, and the source text has to survive all of it.
 */
class PdfExporterTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testPageObjects();
    void testRefusesObjectStreams();
    void testInkLandsWhereItWasDrawn();
    void testSourceTextSurvives();

private:
    QString fixturePath(const QString &name) const
    {
        return QStringLiteral(FILES_DATA_DIR) + name;
    }

    /// A transparent ink plane with one opaque square near the top left of the *displayed* page.
    QImage inkWithMark(const QSize &displaySize) const
    {
        QImage ink(displaySize, QImage::Format_ARGB32);
        ink.fill(Qt::transparent);
        for (int y = 10; y < 60; ++y) {
            for (int x = 10; x < 60; ++x) {
                ink.setPixel(x, y, qRgba(0, 0, 0, 255));
            }
        }
        return ink;
    }

    /// The manifest is what maps a page index to a record, so the test builds a real one rather
    /// than exercising a path the application never takes.
    PdfSessionManifest manifestFor(PdfRenderBackend &backend) const
    {
        PdfSessionManifest manifest;
        manifest.sourceFile = QStringLiteral("text-fixture.pdf");
        manifest.sourceSha256 = QByteArrayLiteral("0000");
        manifest.sourceByteSize = 1;
        for (int i = 0; i < backend.pageCount(); ++i) {
            const PdfPageInfo info = backend.pageInfo(i);
            manifest.pages.append(PdfPageRecord{i, info.sizePt, info.rotation,
                                                PdfSession::pageFileName(i), QString(), 0});
        }
        return manifest;
    }

    QRect darkBounds(const QImage &image) const
    {
        int x0 = image.width();
        int y0 = image.height();
        int x1 = -1;
        int y1 = -1;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (qAlpha(image.pixel(x, y)) > 0 && qGray(image.pixel(x, y)) < 200) {
                    x0 = qMin(x0, x);
                    y0 = qMin(y0, y);
                    x1 = qMax(x1, x);
                    y1 = qMax(y1, y);
                }
            }
        }
        return x1 < 0 ? QRect() : QRect(QPoint(x0, y0), QPoint(x1, y1));
    }
};

void PdfExporterTest::testPageObjects()
{
    QFile file(fixturePath(QStringLiteral("text-fixture.pdf")));
    QVERIFY(file.open(QIODevice::ReadOnly));

    QString why;
    const QList<int> pages = PdfExporter::pageObjectNumbers(file.readAll(), &why);
    QVERIFY2(pages.size() == 3, qPrintable(why));
    QCOMPARE(pages, QList<int>({3, 5, 7}));
}

void PdfExporterTest::testRefusesObjectStreams()
{
    const QByteArray streamed = "%PDF-1.5\n1 0 obj << /Type /ObjStm >> endobj\n";
    QString why;
    QVERIFY(PdfExporter::pageObjectNumbers(streamed, &why).isEmpty());
    QVERIFY(why.contains(QStringLiteral("object streams")));
}

void PdfExporterTest::testInkLandsWhereItWasDrawn()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath(QStringLiteral("text-fixture.pdf"))));

    QHash<int, QImage> ink;
    for (int i = 0; i < backend.pageCount(); ++i) {
        const QSizeF sizePt = backend.pageInfo(i).sizePt;
        ink.insert(i, inkWithMark(QSize(qRound(sizePt.width()), qRound(sizePt.height()))));
    }

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("exported.pdf"));

    QString why;
    QVERIFY2(PdfExporter::exportWithInk(fixturePath(QStringLiteral("text-fixture.pdf")),
                                        manifestFor(backend), ink, out, &why),
             qPrintable(why));
    Q_UNUSED(why);

    PopplerRenderBackend exported;
    QVERIFY(exported.open(out));
    QCOMPARE(exported.pageCount(), 3);

    /// The mark was drawn near the top left of every displayed page, so it has to come back
    /// near the top left of every rendered page, rotation included.
    for (int i = 0; i < exported.pageCount(); ++i) {
        const QImage rendered = exported.renderPage(i, 72.0);
        QVERIFY(!rendered.isNull());

        const QRect bounds = darkBounds(rendered);
        QVERIFY2(bounds.isValid(), "no ink appeared at all");

        const QRect expected(10, 10, 50, 50);
        const QString where = QStringLiteral("page %1: ink at (%2,%3)-(%4,%5), expected about (%6,%7)")
                                  .arg(i + 1)
                                  .arg(bounds.left()).arg(bounds.top())
                                  .arg(bounds.right()).arg(bounds.bottom())
                                  .arg(expected.left()).arg(expected.top());
        QVERIFY2(qAbs(bounds.left() - expected.left()) <= 4
                     && qAbs(bounds.top() - expected.top()) <= 4,
                 qPrintable(where));
    }
}

void PdfExporterTest::testSourceTextSurvives()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath(QStringLiteral("text-fixture.pdf"))));

    QHash<int, QImage> ink;
    ink.insert(0, inkWithMark(QSize(595, 842)));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("exported.pdf"));

    QString why;
    QVERIFY2(PdfExporter::exportWithInk(fixturePath(QStringLiteral("text-fixture.pdf")),
                                        manifestFor(backend), ink, out, &why),
             qPrintable(why));

    PopplerRenderBackend exported;
    QVERIFY(exported.open(out));

    /// No original object is rewritten, so the text has to still be selectable.
    const QString text = exported.pageText(0);
    QVERIFY2(text.contains(QStringLiteral("Page one heading")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("Plain A4 page.")), qPrintable(text));
}

QTEST_MAIN(PdfExporterTest)
#include "PdfExporterTest.moc"
