/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "backends/poppler/PopplerRenderBackend.h"
#include "session/PdfSession.h"

#include <QTemporaryDir>
#include <QtTest>

/**
 * The manifest is the one durable description of a note project, and the source checksum is
 * what makes "the file underneath me changed" detectable instead of silently wrong. Both are
 * pure logic and belong in ctest.
 */
class PdfSessionTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testCreateProject();
    void testSourceIsCopiedUnchanged();
    void testManifestRoundTrip();
    void testRefusesToClobber();
    void testDetectsChangedSource();
    void testRejectsBadManifest();

private:
    QString fixturePath(const QString &name) const
    {
        return QStringLiteral(FILES_DATA_DIR) + name;
    }
};

void PdfSessionTest::testCreateProject()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    PopplerRenderBackend backend;
    QString why;
    const PdfSessionManifest manifest =
        PdfSession::createProject(dir.filePath(QStringLiteral("project")), fixturePath(QStringLiteral("text-fixture.pdf")), backend, &why);

    QVERIFY2(manifest.isValid(&why), qPrintable(why));
    QCOMPARE(manifest.sourceFile, QStringLiteral("text-fixture.pdf"));
    QCOMPARE(manifest.pages.size(), 3);

    /// Geometry comes from the backend, so the rotated page is recorded rotated.
    QCOMPARE(manifest.pages.at(0).sizePt, QSizeF(595, 842));
    QCOMPARE(manifest.pages.at(0).rotation, 0);
    QCOMPARE(manifest.pages.at(1).sizePt, QSizeF(420, 595));
    QCOMPARE(manifest.pages.at(1).rotation, 90);
    QCOMPARE(manifest.pages.at(2).sizePt, QSizeF(300, 300));

    QCOMPARE(manifest.pages.at(0).kraFile, QStringLiteral("pages/p0001.kra"));
    QCOMPARE(manifest.pages.at(2).thumbFile, QStringLiteral("thumbs/p0003.png"));
}

void PdfSessionTest::testSourceIsCopiedUnchanged()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    PopplerRenderBackend backend;
    const PdfSessionManifest manifest =
        PdfSession::createProject(dir.filePath(QStringLiteral("project")), fixturePath(QStringLiteral("text-fixture.pdf")), backend);

    const QString copied = PdfSession::sourcePath(dir.filePath(QStringLiteral("project")), manifest.sourceFile);
    QVERIFY(QFileInfo::exists(copied));

    /// The source is immutable: the copy has to be byte for byte the original.
    QCOMPARE(PdfSessionManifest::sha256OfFile(copied),
             PdfSessionManifest::sha256OfFile(fixturePath(QStringLiteral("text-fixture.pdf"))));
    QCOMPARE(QFileInfo(copied).size(), manifest.sourceByteSize);
}

void PdfSessionTest::testManifestRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    PopplerRenderBackend backend;
    const QString project = dir.filePath(QStringLiteral("project"));
    const PdfSessionManifest written =
        PdfSession::createProject(project, fixturePath(QStringLiteral("text-fixture.pdf")), backend);

    QVERIFY(QFileInfo::exists(PdfSession::manifestPath(project)));

    QString why;
    const PdfSessionManifest read = PdfSession::openProject(project, &why);
    QVERIFY2(read.isValid(&why), qPrintable(why));
    QCOMPARE(read.toJson(), written.toJson());
    QCOMPARE(read.sourceSha256, written.sourceSha256);
    QCOMPARE(read.sourceByteSize, written.sourceByteSize);
}

void PdfSessionTest::testRefusesToClobber()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    PopplerRenderBackend backend;
    const QString project = dir.filePath(QStringLiteral("project"));
    QVERIFY(PdfSession::createProject(project, fixturePath(QStringLiteral("text-fixture.pdf")), backend).isValid());

    QString why;
    const PdfSessionManifest second =
        PdfSession::createProject(project, fixturePath(QStringLiteral("text-fixture.pdf")), backend, &why);
    QVERIFY(!second.isValid());
    QVERIFY(why.contains(QStringLiteral("not empty")));
}

void PdfSessionTest::testDetectsChangedSource()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    PopplerRenderBackend backend;
    const QString project = dir.filePath(QStringLiteral("project"));
    const PdfSessionManifest manifest =
        PdfSession::createProject(project, fixturePath(QStringLiteral("text-fixture.pdf")), backend);

    const QString source = PdfSession::sourcePath(project, manifest.sourceFile);
    {
        QFile file(source);
        QVERIFY(file.open(QIODevice::Append));
        file.write(" ");
    }

    QString why;
    QVERIFY(!PdfSession::openProject(project, &why).isValid());
    QVERIFY2(why.contains(QStringLiteral("changed")), qPrintable(why));
}

void PdfSessionTest::testRejectsBadManifest()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString path = dir.filePath(QStringLiteral("manifest.json"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("this is not json");
    }

    QString why;
    QVERIFY(!PdfSessionManifest::readFrom(path, &why).isValid());
    QVERIFY(!why.isEmpty());

    PdfSessionManifest wrongSchema;
    wrongSchema.schema = 999;
    wrongSchema.sourceFile = QStringLiteral("x.pdf");
    wrongSchema.sourceSha256 = QByteArrayLiteral("deadbeef");
    wrongSchema.pages.append(PdfPageRecord{0, QSizeF(10, 10), 0, QStringLiteral("pages/p0001.kra"), QString(), 0});
    QVERIFY(!wrongSchema.isValid(&why));
    QVERIFY(why.contains(QStringLiteral("schema")));
}

QTEST_MAIN(PdfSessionTest)
#include "PdfSessionTest.moc"
