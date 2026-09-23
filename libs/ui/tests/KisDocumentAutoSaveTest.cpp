/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <KisDocument.h>
#include <KisPart.h>
#include <KisResourceCacheDb.h>
#include <KisResourceLocator.h>

#include <KoColorSpaceRegistry.h>
#include <KoTestConfig.h>

#include <kis_image.h>

#include <QApplication>
#include <QDir>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTimer>
#include <QtTest>

/**
 * Autosave and recovery are Krita's own, and a notebook page must stay out of them.
 *
 * A page document has no URL. As soon as it is modified Krita starts the autosave timer, and
 * generateAutoSaveFileName() falls back to Krita's default autosave location when there is no path
 * -- so the page would come back in the recovery dialog on the next start, as the whole editing
 * document with the rendered page in it rather than as our ink-only artifact.
 *
 * setAutoSaveActive(false) is the opt-out. This test checks both halves of it: that a document
 * which opted out neither schedules the timer nor writes a file, and -- the control -- that a
 * normal document still does exactly what it did before.
 */
class KisDocumentAutoSaveTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void testNormalDocumentStillSchedulesAutosave();
    void testNormalDocumentStillWritesAnAutosaveFile();
    void testOptedOutDocumentDoesNotScheduleAutosave();
    void testOptedOutDocumentWritesNoAutosaveFile();
    void testDocumentWithoutANameIsStillNotSaved();
    void testUntitledCaptionNamesTheDocument();

private:
    /// Every autosave file Krita would consider on the next start, absolute paths, sorted.
    static QStringList autosaveFiles();

    /// A document with a small image and nothing else, which is all the autosave path needs.
    static KisDocument *makePageDocument();
};

/// Where Krita puts the autosave file of a document that has no path.
///
/// KisAutoSaveRecoveryDialog::autoSaveLocation() is the authority, but it is not exported from
/// kritaui, so a test outside the library cannot call it; this mirrors the branch for the platform
/// the test is built on (KisAutoSaveRecoveryDialog.cpp). If it ever drifts, the control test below
/// fails, because it waits for a file to appear here.
static QString autosaveDirectory()
{
#if defined(Q_OS_WIN)
    return QDir::tempPath();
#elif defined(Q_OS_ANDROID)
    return QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
        .filePath(QStringLiteral("krita-backup"));
#else
    return QDir::homePath();
#endif
}

QStringList KisDocumentAutoSaveTest::autosaveFiles()
{
    const QDir dir(autosaveDirectory());
    QStringList files;
    const QStringList names = dir.entryList(QStringList() << QStringLiteral("*autosave.kra"),
                                            QDir::Files | QDir::Hidden);
    for (const QString &name : names) {
        files << dir.absoluteFilePath(name);
    }
    files.sort();
    return files;
}

KisDocument *KisDocumentAutoSaveTest::makePageDocument()
{
    KisDocument *document = KisPart::instance()->createDocument();
    KisImageSP image = new KisImage(document->createUndoStore(), 32, 32,
                                    KoColorSpaceRegistry::instance()->rgb8(), QStringLiteral("page"));
    document->setCurrentImage(image, false);
    return document;
}

void KisDocumentAutoSaveTest::initTestCase()
{
    /// A KisDocument brings up the resource servers and the resource locator, so the locator has to
    /// have somewhere to look before the first one is made -- the same init Krita's own tests do
    /// (sdk/tests/kistest.h, the TESTUI branch of registerResources).
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QVERIFY(QDir().mkpath(appData));
    if (!KisResourceCacheDb::initialize(appData)) {
        qFatal("could not initialise the resource cache database under %s", qPrintable(appData));
    }
    KisResourceLocator::instance()->initialize(
        QStringLiteral(KRITA_RESOURCE_DIRS_FOR_TESTS).section(QLatin1Char(';'), 0, 0)
        + QStringLiteral("/krita"));
}

/**
 * The control: nothing in this change may have touched a document that Krita owns.
 */
void KisDocumentAutoSaveTest::testNormalDocumentStillSchedulesAutosave()
{
    QScopedPointer<KisDocument> document(makePageDocument());
    QVERIFY(document);
    QVERIFY(document->isAutoSaveActive());
    QVERIFY(document->untitledCaption().isEmpty());

    /// Exactly one timer, and the opt-out is what stops it -- a fresh document already has it
    /// running, which is why the opted-out case below is the one that shows the difference.
    const QList<QTimer *> timers = document->findChildren<QTimer *>();
    QCOMPARE(timers.size(), 1);

    document->setModified(true);

    QVERIFY(document->isModified());
    QVERIFY2(timers.first()->isActive(),
             "a normal modified document no longer schedules autosave");
}

/**
 * And the control for the write: a normal modified document still reaches the autosave file.
 */
void KisDocumentAutoSaveTest::testNormalDocumentStillWritesAnAutosaveFile()
{
    const QStringList before = autosaveFiles();

    QScopedPointer<KisDocument> document(makePageDocument());
    QVERIFY(document);
    document->setModified(true);

    QSignalSpy messages(document.data(), SIGNAL(statusBarMessage(QString,int)));
    QVERIFY(QMetaObject::invokeMethod(document.data(), "slotAutoSave"));

    QTRY_VERIFY_WITH_TIMEOUT(autosaveFiles().size() > before.size(), 30000);
    QVERIFY2(!messages.isEmpty(), "the autosave was not announced");

    /// The test put it there, so the test takes it away again.
    const QStringList after = autosaveFiles();
    for (const QString &file : after) {
        if (!before.contains(file)) {
            QFile::remove(file);
        }
    }
}

/**
 * The opt-out, first half: no timer.
 */
void KisDocumentAutoSaveTest::testOptedOutDocumentDoesNotScheduleAutosave()
{
    QScopedPointer<KisDocument> document(makePageDocument());
    QVERIFY(document);

    document->setAutoSaveActive(false);
    QVERIFY(!document->isAutoSaveActive());

    document->setModified(true);

    /// It is still a modified document; being modified is not what the opt-out changes.
    QVERIFY(document->isModified());

    const QList<QTimer *> timers = document->findChildren<QTimer *>();
    QCOMPARE(timers.size(), 1);
    QVERIFY2(!timers.first()->isActive(),
             "a document that opted out of autosave still scheduled it");
}

/**
 * The opt-out, second half: no file, even if the slot is called directly. This is the path the
 * timer would take, and it is the one that must not write.
 */
void KisDocumentAutoSaveTest::testOptedOutDocumentWritesNoAutosaveFile()
{
    const QStringList before = autosaveFiles();

    QScopedPointer<KisDocument> document(makePageDocument());
    QVERIFY(document);
    document->setAutoSaveActive(false);
    document->setModified(true);

    QSignalSpy messages(document.data(), SIGNAL(statusBarMessage(QString,int)));
    QVERIFY(QMetaObject::invokeMethod(document.data(), "slotAutoSave"));

    /// The other test waits for a background save to land; this waits longer than that, so
    /// "no file" is an answer rather than impatience.
    QTest::qWait(3000);

    QCOMPARE(autosaveFiles(), before);
    for (const QList<QVariant> &message : messages) {
        QVERIFY2(!message.at(0).toString().contains(QStringLiteral("Autosaving")),
                 qPrintable(message.at(0).toString()));
    }
}

void KisDocumentAutoSaveTest::testDocumentWithoutANameIsStillNotSaved()
{
    QScopedPointer<KisDocument> document(makePageDocument());
    QVERIFY(document);
    QVERIFY(document->untitledCaption().isEmpty());

    /// Krita's own words for a document with no file, unchanged.
    QVERIFY2(document->caption().contains(QStringLiteral("Not Saved")),
             qPrintable(document->caption()));
}

void KisDocumentAutoSaveTest::testUntitledCaptionNamesTheDocument()
{
    QScopedPointer<KisDocument> document(makePageDocument());
    QVERIFY(document);

    document->setUntitledCaption(QStringLiteral("My notebook - page 3/10"));
    QCOMPARE(document->untitledCaption(), QStringLiteral("My notebook - page 3/10"));

    /// What the tab and the window title are built from (KisView::slotUpdateDocumentTitle()).
    QCOMPARE(document->caption(), QStringLiteral("My notebook - page 3/10"));

    /// And a document that does have a file is still named after the file.
    document->setPath(QStringLiteral("/tmp/somewhere/notebook-page.kra"));
    QCOMPARE(document->caption(), QStringLiteral("notebook-page.kra"));
}

int main(int argc, char *argv[])
{
    qputenv("LANGUAGE", "en");
    QStandardPaths::setTestModeEnabled(true);

    /// The autosave write is Krita's .kra export, which is a plugin, and the colour space registry
    /// wants the ICC engine from another. Both are pointed at the directory they are built into.
    qputenv("KRITA_PLUGIN_PATH", QByteArray(KISDOCUMENT_TEST_PLUGIN_DIR));
    qputenv("EXTRA_RESOURCE_DIRS", QByteArray(KRITA_RESOURCE_DIRS_FOR_TESTS));

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("KisDocumentAutoSaveTest"));

    KisDocumentAutoSaveTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "KisDocumentAutoSaveTest.moc"
