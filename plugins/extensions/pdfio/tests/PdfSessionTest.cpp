/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "backends/poppler/PopplerRenderBackend.h"
#include "session/PdfSession.h"

#include <QDir>
#include <QStandardPaths>
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
    void initTestCase();
    void cleanupTestCase();

    void testCreateProject();
    void testSourceIsCopiedUnchanged();
    void testManifestRoundTrip();
    void testRefusesToClobber();
    void testDetectsChangedSource();
    void testRejectsBadManifest();

    // Where the notebook folder is, and what happens to an older one when it moves.
    void testProjectRootIsUnderDocuments();
    void testProjectRootFallsBackWhenDocumentsIsUnusable();
    void testLegacyNotebookIsMovedAndStillOpens();
    void testFailedMoveLeavesTheLegacyNotebookIntact();
    void testMigratedManifestKeepsRelativePaths();

private:
    QString fixturePath(const QString &name) const
    {
        return QStringLiteral(FILES_DATA_DIR) + name;
    }

    /**
     * The parent a case's Documents directory is made under: beside the legacy root, so both are
     * on one filesystem.
     *
     * The migration is an atomic rename, and rename(2) cannot cross a filesystem boundary. /tmp is
     * usually a different one (tmpfs here), so a QTemporaryDir there would fail every migration
     * case for a reason that has nothing to do with the policy. Production puts Documents and the
     * app data directory under the same home, which is one filesystem.
     */
    QString temporaryDocumentsParent() const
    {
        const QString parent =
            QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                .filePath(QStringLiteral("documents"));
        QDir().mkpath(parent);
        return parent;
    }

    /// A notebook built at the legacy root, which is the state a migration starts from. Returns an
    /// empty string on failure, with the reason in \a why.
    QString makeLegacyNotebook(const QString &name, QString *why)
    {
        const QString dir = QDir(PdfSession::legacyProjectRoot()).filePath(name);
        QDir(dir).removeRecursively();

        PopplerRenderBackend backend;
        const PdfSessionManifest manifest =
            PdfSession::createProject(dir, fixturePath(QStringLiteral("text-fixture.pdf")), backend, why);
        return manifest.isValid(why) ? dir : QString();
    }
};

void PdfSessionTest::initTestCase()
{
    /// The legacy root is AppDataLocation/pdfio-projects. Test mode keeps that under ~/.qttest, so
    /// a migration case cannot touch a real notebook store.
    QStandardPaths::setTestModeEnabled(true);
}

void PdfSessionTest::cleanupTestCase()
{
    PdfSession::setDocumentsLocationForTests(QString());
    QDir(QDir(PdfSession::legacyProjectRoot()).absoluteFilePath(QStringLiteral("../documents")))
        .removeRecursively();
    QDir(PdfSession::legacyProjectRoot()).removeRecursively();
}

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

/**
 * The notebook folder is somewhere the user can see, and older notebooks have to survive the move.
 *
 * The Documents location is pointed at a temporary directory for these cases; the policy reads it
 * through PdfSession::setDocumentsLocationForTests(), which nothing in the plugin calls.
 */
void PdfSessionTest::testProjectRootIsUnderDocuments()
{
    QTemporaryDir documents(temporaryDocumentsParent() + QStringLiteral("/case-XXXXXX"));
    QVERIFY(documents.isValid());
    PdfSession::setDocumentsLocationForTests(documents.path());

    const QString root = PdfSession::projectRoot();

    /// One folder directly under Documents -- no longer the app-private root, and not a path
    /// spelled out here, because the folder's name is the policy's to choose.
    QCOMPARE(QFileInfo(root).absolutePath(), QDir(documents.path()).absolutePath());
    QVERIFY(!QFileInfo(root).fileName().isEmpty());
    QVERIFY(root != PdfSession::legacyProjectRoot());

    /// And that is where a notebook that exists nowhere yet is about to be made.
    QCOMPARE(PdfSession::migrateFromLegacy(QStringLiteral("brand-new")),
             QDir(root).filePath(QStringLiteral("brand-new")));

    PdfSession::setDocumentsLocationForTests(QString());
}

void PdfSessionTest::testProjectRootFallsBackWhenDocumentsIsUnusable()
{
    /// A Documents location that is not there at all.
    const QString missing = QDir(QDir::tempPath()).filePath(QStringLiteral("pdfsession-nothing-here"));
    QDir(missing).removeRecursively();
    QVERIFY(!QFileInfo::exists(missing));
    PdfSession::setDocumentsLocationForTests(missing);
    QCOMPARE(PdfSession::projectRoot(), PdfSession::legacyProjectRoot());

    /// And one that is there and cannot be written to. This case would not hold for a run as root,
    /// where nothing is unwritable.
    QTemporaryDir unwritable;
    QVERIFY(unwritable.isValid());
    QVERIFY(QFile::setPermissions(unwritable.path(),
                                  QFile::ReadOwner | QFile::ReadUser
                                      | QFile::ExeOwner | QFile::ExeUser));
    QVERIFY(!QFileInfo(unwritable.path()).isWritable());
    PdfSession::setDocumentsLocationForTests(unwritable.path());
    QCOMPARE(PdfSession::projectRoot(), PdfSession::legacyProjectRoot());

    QFile::setPermissions(unwritable.path(),
                          QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser
                              | QFile::ExeOwner | QFile::ExeUser);
    PdfSession::setDocumentsLocationForTests(QString());
}

void PdfSessionTest::testLegacyNotebookIsMovedAndStillOpens()
{
    QTemporaryDir documents(temporaryDocumentsParent() + QStringLiteral("/case-XXXXXX"));
    QVERIFY(documents.isValid());
    PdfSession::setDocumentsLocationForTests(documents.path());

    QString why;
    const QString legacyDir = makeLegacyNotebook(QStringLiteral("legacy-moved"), &why);
    QVERIFY2(!legacyDir.isEmpty(), qPrintable(why));
    QVERIFY(QFileInfo::exists(PdfSession::manifestPath(legacyDir)));

    const QString moved = PdfSession::migrateFromLegacy(QStringLiteral("legacy-moved"), &why);
    QCOMPARE(moved, QDir(PdfSession::projectRoot()).filePath(QStringLiteral("legacy-moved")));

    /// Moved, not copied: the old root no longer holds it, and there is exactly one copy.
    QVERIFY(!QFileInfo::exists(PdfSession::manifestPath(legacyDir)));
    QVERIFY(QFileInfo::exists(PdfSession::manifestPath(moved)));

    /// And it is still a notebook: the source came with it and the manifest still opens.
    QVERIFY2(PdfSession::openProject(moved, &why).isValid(&why), qPrintable(why));

    /// Asking again changes nothing.
    QCOMPARE(PdfSession::migrateFromLegacy(QStringLiteral("legacy-moved"), &why), moved);

    QDir(moved).removeRecursively();
    QDir(legacyDir).removeRecursively();
    PdfSession::setDocumentsLocationForTests(QString());
}

void PdfSessionTest::testFailedMoveLeavesTheLegacyNotebookIntact()
{
    QTemporaryDir documents(temporaryDocumentsParent() + QStringLiteral("/case-XXXXXX"));
    QVERIFY(documents.isValid());
    PdfSession::setDocumentsLocationForTests(documents.path());

    QString why;
    const QString legacyDir = makeLegacyNotebook(QStringLiteral("legacy-refused"), &why);
    QVERIFY2(!legacyDir.isEmpty(), qPrintable(why));

    /// A plain file where the moved notebook would have to go, so the rename cannot be made. What
    /// has to happen then is that the notebook stays whole where it is.
    const QString blocked = QDir(PdfSession::projectRoot()).filePath(QStringLiteral("legacy-refused"));
    QVERIFY(QDir().mkpath(PdfSession::projectRoot()));
    {
        QFile file(blocked);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("not a notebook"), qint64(14));
    }

    why.clear();
    QCOMPARE(PdfSession::migrateFromLegacy(QStringLiteral("legacy-refused"), &why), legacyDir);
    QVERIFY2(!why.isEmpty(), "a refused move has to say why");

    /// The legacy copy is untouched, source included.
    const PdfSessionManifest manifest =
        PdfSessionManifest::readFrom(PdfSession::manifestPath(legacyDir));
    QVERIFY(manifest.isValid());
    QVERIFY(QFileInfo::exists(PdfSession::sourcePath(legacyDir, manifest.sourceFile)));

    /// And it is still openable where it is -- that is what returning the legacy directory means.
    why.clear();
    QVERIFY2(PdfSession::openProject(legacyDir, &why).isValid(&why), qPrintable(why));

    QFile::remove(blocked);
    QDir(legacyDir).removeRecursively();
    PdfSession::setDocumentsLocationForTests(QString());
}

void PdfSessionTest::testMigratedManifestKeepsRelativePaths()
{
    QTemporaryDir documents(temporaryDocumentsParent() + QStringLiteral("/case-XXXXXX"));
    QVERIFY(documents.isValid());
    PdfSession::setDocumentsLocationForTests(documents.path());

    QString why;
    const QString legacyDir = makeLegacyNotebook(QStringLiteral("legacy-paths"), &why);
    QVERIFY2(!legacyDir.isEmpty(), qPrintable(why));

    const QString moved = PdfSession::migrateFromLegacy(QStringLiteral("legacy-paths"), &why);
    QVERIFY2(moved != legacyDir, qPrintable(why));

    const PdfSessionManifest manifest = PdfSession::openProject(moved, &why);
    QVERIFY2(manifest.isValid(&why), qPrintable(why));

    /// A notebook is portable only while its manifest holds relative paths, and a migration is a
    /// chance to write an absolute one by accident.
    QVERIFY(!QDir::isAbsolutePath(manifest.sourceFile));
    QVERIFY(!manifest.sourceFile.contains(QLatin1Char('/')));
    for (const PdfPageRecord &page : manifest.pages) {
        QVERIFY2(!QDir::isAbsolutePath(page.kraFile), qPrintable(page.kraFile));
        QVERIFY2(!QDir::isAbsolutePath(page.thumbFile), qPrintable(page.thumbFile));
        QVERIFY2(!page.kraFile.startsWith(QStringLiteral("..")), qPrintable(page.kraFile));
        QVERIFY2(!page.thumbFile.startsWith(QStringLiteral("..")), qPrintable(page.thumbFile));
    }

    /// And the file on disk says the same: neither root left a path behind in it.
    QFile raw(PdfSession::manifestPath(moved));
    QVERIFY(raw.open(QIODevice::ReadOnly));
    const QByteArray json = raw.readAll();
    QVERIFY(!json.contains(moved.toUtf8()));
    QVERIFY(!json.contains(documents.path().toUtf8()));
    QVERIFY(!json.contains(PdfSession::legacyProjectRoot().toUtf8()));

    /// The paths still resolve, relative to the notebook that moved.
    QVERIFY(QFileInfo::exists(PdfSession::sourcePath(moved, manifest.sourceFile)));

    QDir(moved).removeRecursively();
    QDir(legacyDir).removeRecursively();
    PdfSession::setDocumentsLocationForTests(QString());
}

QTEST_MAIN(PdfSessionTest)
#include "PdfSessionTest.moc"
