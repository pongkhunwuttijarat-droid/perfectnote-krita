/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "backends/poppler/PopplerRenderBackend.h"
#include "session/PdfSession.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest>

namespace {

void writeBytes(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    file.write(bytes);
}

} // namespace

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

    // Where a manifest's own file names are checked: once, at the boundary, so that no consumer
    // has to, and so a directory copied onto the machine cannot make one read or write outside it.
    void testRejectsEscapingManifestPaths();
    void testAcceptsLegitimateFileNames();
    void testEmptyThumbnailStaysLegal();
    void testOpenProjectRefusesAnEscapingManifest();
    void testPathInsideProject();

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

    /// A manifest that is valid in every way except whatever a case changes: one page, ordinary
    /// names, a checksum that is only ever compared against itself in these cases.
    PdfSessionManifest baseManifest() const
    {
        PdfSessionManifest manifest;
        manifest.schema = PdfSessionManifest::CurrentSchema;
        manifest.sourceFile = QStringLiteral("source.pdf");
        manifest.sourceSha256 = QByteArrayLiteral("deadbeef");
        manifest.sourceByteSize = 4;
        manifest.pages.append(PdfPageRecord{0, QSizeF(595, 842), 0,
                                            QStringLiteral("pages/p0001.kra"),
                                            QStringLiteral("thumbs/p0001.png"), 0});
        return manifest;
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

void PdfSessionTest::testRejectsEscapingManifestPaths()
{
    /// One case per escape shape, on the rule itself first: what is being refused is a name that
    /// would leave the project directory once somebody joins it.
    const QList<QPair<QString, QString>> escapes = {
        { QStringLiteral("absolute"), QStringLiteral("/etc/passwd") },
        { QStringLiteral("absolute-in-tmp"), QStringLiteral("/tmp/source.pdf") },
        { QStringLiteral("drive"), QStringLiteral("C:/source.pdf") },
        { QStringLiteral("drive-relative"), QStringLiteral("C:source.pdf") },
        { QStringLiteral("backslash"), QStringLiteral("pages\\..\\source.pdf") },
        { QStringLiteral("parent"), QStringLiteral("../source.pdf") },
        { QStringLiteral("parent-inside"), QStringLiteral("pages/../../source.pdf") },
        { QStringLiteral("empty-component"), QStringLiteral("pages//p0001.kra") },
        { QStringLiteral("dot-component"), QStringLiteral("./source.pdf") },
        { QStringLiteral("empty"), QString() },
    };

    for (const QPair<QString, QString> &escape : escapes) {
        QString why;
        QVERIFY2(!PdfSessionManifest::isSafeRelativePath(escape.second, &why), qPrintable(escape.first));
        QVERIFY2(!why.isEmpty(), qPrintable(escape.first));
    }

    /// And through the manifest, where the field and its value have to be named in the reason.
    for (const QPair<QString, QString> &escape : escapes) {
        if (escape.second.isEmpty()) {
            continue;   ///< empty has its own message, checked below
        }
        PdfSessionManifest manifest = baseManifest();
        manifest.sourceFile = escape.second;

        QString why;
        QVERIFY2(!manifest.isValid(&why), qPrintable(escape.first));
        QVERIFY2(why.contains(QStringLiteral("manifest's source file")), qPrintable(escape.first + ": " + why));
        QVERIFY2(why.contains(escape.second), qPrintable(escape.first + ": " + why));
    }

    /// Empty where a name is required: the source, and a page's ink. Both are refused, and the
    /// page one names the page.
    {
        PdfSessionManifest manifest = baseManifest();
        manifest.sourceFile.clear();
        QString why;
        QVERIFY(!manifest.isValid(&why));
        QVERIFY2(why.contains(QStringLiteral("no source file recorded")), qPrintable(why));

        manifest = baseManifest();
        manifest.pages[0].kraFile.clear();
        why.clear();
        QVERIFY(!manifest.isValid(&why));
        QVERIFY2(why.contains(QStringLiteral("ink file is not recorded")), qPrintable(why));
    }

    /// The page fields carry the same rule, with the page and the field named.
    {
        PdfSessionManifest manifest = baseManifest();
        manifest.pages[0].kraFile = QStringLiteral("/tmp/ink-escape.kra");
        QString why;
        QVERIFY(!manifest.isValid(&why));
        QVERIFY2(why.contains(QStringLiteral("manifest's page 1 ink file")), qPrintable(why));
        QVERIFY2(why.contains(QStringLiteral("/tmp/ink-escape.kra")), qPrintable(why));

        manifest = baseManifest();
        manifest.pages[0].thumbFile = QStringLiteral("../../thumb-escape.png");
        why.clear();
        QVERIFY(!manifest.isValid(&why));
        QVERIFY2(why.contains(QStringLiteral("manifest's page 1 thumbnail")), qPrintable(why));
        QVERIFY2(why.contains(QStringLiteral("escape")), qPrintable(why));
    }

    /// The entry point: a manifest.json on disk that names an outside source. This is the
    /// hand-placed project directory the whole change is about, and readFrom() is where it enters.
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        PdfSessionManifest manifest = baseManifest();
        manifest.sourceFile = QStringLiteral("../outside.pdf");
        const QString path = dir.filePath(QStringLiteral("manifest.json"));
        writeBytes(path, QJsonDocument(manifest.toJson()).toJson());

        QString why;
        const PdfSessionManifest read = PdfSessionManifest::readFrom(path, &why);
        QVERIFY(!read.isValid(&why));
        QVERIFY2(why.contains(QStringLiteral("manifest's source file")), qPrintable(why));
        QVERIFY2(why.contains(QStringLiteral("../outside.pdf")), qPrintable(why));
    }
}

void PdfSessionTest::testAcceptsLegitimateFileNames()
{
    /// Real names are not ASCII and are not short. A case here has to pass the rule and survive the
    /// JSON round trip, or the validation would be refusing notebooks that work today.
    const QStringList sources = {
        QStringLiteral("source.pdf"),
        QStringLiteral("my notes.pdf"),
        QStringLiteral("source (copy).pdf"),
        QStringLiteral("v1.2.3.pdf"),
        QStringLiteral("2026-09-23T12.30.45+07.00.pdf"),
        QStringLiteral("บันทึกของฉัน.pdf"),
        QStringLiteral("ページ.ノート.2026.pdf"),
        QStringLiteral("sub/dir/source.pdf"),
    };

    for (const QString &source : sources) {
        PdfSessionManifest manifest = baseManifest();
        manifest.sourceFile = source;

        QString why;
        QVERIFY2(manifest.isValid(&why), qPrintable(source + ": " + why));

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("manifest.json"));
        QVERIFY2(manifest.writeTo(path, &why), qPrintable(source + ": " + why));
        why.clear();
        const PdfSessionManifest read = PdfSessionManifest::readFrom(path, &why);
        QVERIFY2(read.isValid(&why), qPrintable(source + ": " + why));
        QCOMPARE(read.sourceFile, source);
    }

    const QStringList inkNames = {
        QStringLiteral("pages/p0001.kra"),
        QStringLiteral("pages/my notes 01.kra"),
        QStringLiteral("ink/ページ1.kra"),
    };
    for (const QString &ink : inkNames) {
        PdfSessionManifest manifest = baseManifest();
        manifest.pages[0].kraFile = ink;
        manifest.pages[0].thumbFile = QStringLiteral("thumbs/บันทึก 01.png");
        QString why;
        QVERIFY2(manifest.isValid(&why), qPrintable(ink + ": " + why));
    }
}

void PdfSessionTest::testEmptyThumbnailStaysLegal()
{
    /// The decision: an empty thumbnail means "no preview made yet". Refusing it would stop
    /// notebooks that open today from opening, and the exporter and strip tests build exactly such
    /// manifests. A thumbnail that is there is checked like any other name, because the docker, the
    /// strip decoration and the bundle all join it.
    PdfSessionManifest manifest = baseManifest();
    manifest.pages[0].thumbFile.clear();

    QString why;
    QVERIFY2(manifest.isValid(&why), qPrintable(why));

    manifest.pages[0].thumbFile = QStringLiteral("../thumbs/p0001.png");
    why.clear();
    QVERIFY(!manifest.isValid(&why));
    QVERIFY2(why.contains(QStringLiteral("manifest's page 1 thumbnail")), qPrintable(why));
}

void PdfSessionTest::testOpenProjectRefusesAnEscapingManifest()
{
    /// A project directory copied onto the machine by hand, whose manifest names a source outside
    /// itself. The file exists and hashes to exactly what the manifest records, so nothing but the
    /// boundary check stands between it and being read.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString project = dir.filePath(QStringLiteral("project"));
    QVERIFY(QDir().mkpath(project));

    const QByteArray bytes = QByteArrayLiteral("the bytes the manifest hashes");
    const QString outside = dir.filePath(QStringLiteral("outside.pdf"));
    writeBytes(outside, bytes);

    PdfSessionManifest manifest = baseManifest();
    manifest.sourceFile = outside;
    manifest.sourceSha256 = PdfSessionManifest::sha256OfFile(outside);
    manifest.sourceByteSize = bytes.size();
    writeBytes(QDir(project).filePath(QStringLiteral("manifest.json")),
               QJsonDocument(manifest.toJson()).toJson());

    QString why;
    const PdfSessionManifest opened = PdfSession::openProject(project, &why);
    QVERIFY(!opened.isValid());
    QVERIFY2(why.contains(QStringLiteral("manifest's source file")), qPrintable(why));
    QVERIFY2(why.contains(QStringLiteral("absolute")), qPrintable(why));
}

void PdfSessionTest::testPathInsideProject()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath(QStringLiteral("project"));

    QString why;
    QVERIFY2(PdfSession::isPathInsideProject(project, QStringLiteral("source.pdf"), &why), qPrintable(why));
    QVERIFY(PdfSession::isPathInsideProject(project, QStringLiteral("pages/p0001.kra"), &why));
    QVERIFY(PdfSession::isPathInsideProject(project, QStringLiteral("sub dir/บันทึก 01.kra"), &why));

    QVERIFY(!PdfSession::isPathInsideProject(project, QStringLiteral("/etc/passwd"), &why));
    QVERIFY(!PdfSession::isPathInsideProject(project, QStringLiteral("../outside.pdf"), &why));
    QVERIFY(!PdfSession::isPathInsideProject(project, QStringLiteral("pages/../../outside.pdf"), &why));
    QVERIFY(!PdfSession::isPathInsideProject(project, QString(), &why));
    /// The project directory itself is not a file inside it.
    QVERIFY(!PdfSession::isPathInsideProject(project, QStringLiteral("."), &why));
}

QTEST_MAIN(PdfSessionTest)
#include "PdfSessionTest.moc"
