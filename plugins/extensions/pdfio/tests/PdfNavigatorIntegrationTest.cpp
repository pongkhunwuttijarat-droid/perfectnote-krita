/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfPageNavigator.h"

#include "session/PdfInkLoader.h"
#include "session/PdfProjectBuilder.h"
#include "session/PdfSession.h"

#include <KisDocument.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisResourceCacheDb.h>
#include <KisResourceLocator.h>
#include <KisView.h>

#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>

#include <KoColor.h>

#include <KoTestConfig.h>

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

/**
 * The page turn, over a real notebook, with a real document behind every page.
 *
 * PdfPageWindowTest proves the eviction rule on its own -- a dirty page is never dropped -- and
 * PdfProjectBuilderTest proves the layer contract, but neither ever turns a page. Everything the
 * rule protects against is a property of the turn as a whole: the document's own modified flag is
 * what the window is told, the save that has to happen before an eviction is the plugin's
 * ink-only save to a real artifact, and "the page turn was refused" only means something if the
 * page that was open is still the page that is open, still holding its ink.
 *
 * So this test does what the plugin does: it wraps a fixture PDF into a notebook the way
 * PdfIoPlugin does, lets PdfPageNavigator build its real KisDocument through KisPart, draws on
 * the page's own ink layer, and then turns pages with next()/previous()/showPage(). Each test
 * asserts the visible outcome -- the open page, KisDocument::isModified(), the artifact on disk
 * -- and the window's counters, which is the part that explains it.
 *
 * What it is not: a brush stroke. The mark is placed straight into the Ink layer's paint device
 * and the modified flag is set through KisDocument::setModified(), which is the flag showPage()
 * reads. The painting pipeline is covered by Krita's own tests; what is under test here is what
 * happens to a page that carries ink when it has to make room.
 */
class PdfNavigatorIntegrationTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    void testCleanTurnEvictsWithoutSaving();
    void testDirtyPageIsSavedBeforeEvictionAndTheInkComesBack();
    void testRefusedSaveKeepsThePageOpenAndTheInkIntact();

private:
    PdfPageNavigator *navigator() const;

    /// Opens a notebook from its own copy of the fixture. The project directory is named after
    /// the source file, so a differently named copy is a different notebook: each test starts
    /// with an empty page store rather than whatever the test before it wrote.
    bool useNotebook(const QString &name);

    /// Paints a black square on the open page's ink layer and marks the document modified.
    void drawInk(KisDocument *document);

    QString artifactFor(int index) const;
    bool waitForInk(const QString &path, int timeoutMs = 30000);

    QTemporaryDir m_dir;
    QString m_fixture;
    KisMainWindow *m_mainWindow = nullptr;
    QTimer *m_dialogWatchdog = nullptr;
};

namespace {

/// A square of ink in the page's own pixels, small enough to be cheap to save and specific
/// enough that finding it back is not a coincidence.
const QRect InkMark(8, 8, 24, 24);

QString projectsRoot()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("pdfio-projects"));
}

KisPaintLayer *inkLayer(const KisImageSP &image)
{
    return qobject_cast<KisPaintLayer *>(PdfProjectBuilder::inkStrokeLayer(image).data());
}

/**
 * Whether the mark is on the page, and whether the rest of the page is still blank.
 *
 * The second half matters: an artifact read back as an opaque page would satisfy a check that
 * only looked for dark pixels, and the test would then pass without any ink having survived.
 */
bool inkMarkPresent(const KisImageSP &image)
{
    KisPaintLayer *layer = inkLayer(image);
    if (!layer || !image) {
        return false;
    }

    const QImage pixels = layer->paintDevice()->convertToQImage(0, image->bounds());
    if (pixels.isNull() || !pixels.rect().contains(InkMark.center())) {
        return false;
    }

    const QColor mark = pixels.pixelColor(InkMark.center());
    const QColor away = pixels.pixelColor(2, 2);
    const bool marked = mark.alpha() > 0 && qGray(mark.rgb()) < 96;
    const bool blankElsewhere = away.alpha() == 0 || qGray(away.rgb()) > 200;
    return marked && blankElsewhere;
}

} // namespace

PdfPageNavigator *PdfNavigatorIntegrationTest::navigator() const
{
    return PdfPageNavigator::instance();
}

void PdfNavigatorIntegrationTest::initTestCase()
{
    /// The main window is built from Krita's own resources -- the XMLGUI file that gives it a
    /// toolbar, and the configuration it starts from. They are not in kritaui, so the test has to
    /// pull them in itself, exactly as Krita's ui tests do (libs/ui/tests/kis_view_signals_test.cpp).
    Q_INIT_RESOURCE(krita);

    QVERIFY(m_dir.isValid());

    m_fixture = QStringLiteral(FILES_DATA_DIR) + QStringLiteral("text-fixture.pdf");
    QVERIFY2(QFileInfo::exists(m_fixture), qPrintable(m_fixture));

    /// Test mode keeps the notebook store under ~/.qttest instead of the user's data directory,
    /// and the store is emptied first: a project directory is derived from its source, so an
    /// artifact left by an earlier run would otherwise be read back as this run's ink.
    QVERIFY(QStandardPaths::isTestModeEnabled());

    /// Krita's own ui tests put the resource system up before they touch a document
    /// (sdk/tests/kistest.h, the TESTUI branch of registerResources); a document created without
    /// it would have a resource locator with nowhere to look. The database lives in the
    /// test-mode application data directory, so the run throws it away with the rest.
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QVERIFY(QDir().mkpath(appData));
    if (!KisResourceCacheDb::initialize(appData)) {
        qFatal("could not initialise the resource cache database under %s", qPrintable(appData));
    }
    KisResourceLocator::instance()->initialize(
        QStringLiteral(KRITA_RESOURCE_DIRS_FOR_TESTS).section(QLatin1Char(';'), 0, 0)
        + QStringLiteral("/krita"));

    QDir store(projectsRoot());
    if (store.exists()) {
        QVERIFY(store.removeRecursively());
    }

    /// A dialog nobody can dismiss hangs a headless run, and the first one can arrive before the
    /// main window even exists, so this goes up before anything that can raise one. The
    /// navigator's own answer is the value under test; the test closes the dialog rather than
    /// waiting behind it.
    m_dialogWatchdog = new QTimer(this);
    m_dialogWatchdog->setInterval(10);
    connect(m_dialogWatchdog, &QTimer::timeout, this, []() {
        const QList<QWidget *> widgets = QApplication::topLevelWidgets();
        for (QWidget *widget : widgets) {
            if (widget->isVisible() && qobject_cast<QDialog *>(widget)) {
                qWarning("[nav-integration] dismissing modal dialog \"%s\"",
                         qPrintable(widget->windowTitle()));
                widget->close();
            }
        }
    });
    m_dialogWatchdog->start();

    /// The page turn needs a document and a view, and the plugin gets both from the current main
    /// window. Krita's own ui tests build one headlessly the same way
    /// (libs/ui/tests/kis_view_signals_test.cpp).
    qWarning("[nav-integration] creating a main window");
    m_mainWindow = KisPart::instance()->createMainWindow();
    qWarning("[nav-integration] main window created: %s", m_mainWindow ? "yes" : "no");
    QVERIFY(m_mainWindow);

    /// Turning a page by panning is driven by a timer watching the middle of the viewport. The
    /// canvas here has no meaningful viewport, and the timer must not turn a page while a
    /// refused save is being handled.
    navigator()->setScrollFollowEnabled(false);
}

void PdfNavigatorIntegrationTest::cleanupTestCase()
{
    /// The test is over and the notebook store is about to be removed, so nothing may be written
    /// on the way out. A document that is still modified makes KisView::queryClose() put up a
    /// "the document has been modified, do you want to save it?" dialog, and a dialog raised from
    /// inside the teardown is a hang or a crash rather than a test result.
    if (KisDocument *document = navigator()->currentDocument()) {
        document->setModified(false);
    }

    /// Krita's own ui tests close the view and delete the main window themselves
    /// (libs/ui/tests/kis_view_signals_test.cpp); leaving the documents, the views and the window
    /// to static destruction is what turned a green run into a segfault on the way out of the
    /// process, after the tests had already reported.
    ///
    /// The view manager is deliberately left in place: closing a view makes Krita's MDI area
    /// activate another one, and KisView::notifyCurrentStateChanged() reaches the input manager
    /// through it. With the manager detached (KisView::setViewManager(nullptr), which is what
    /// libs/ui/tests does) KisView::globalInputManager() returns null and that path crashes.
    if (KisView *view = navigator()->currentView()) {
        view->closeView();
        QApplication::sendPostedEvents();
        QApplication::processEvents();
    }

    if (m_mainWindow) {
        m_mainWindow->hide();
        QApplication::processEvents();
        delete m_mainWindow;
        m_mainWindow = nullptr;
        QApplication::sendPostedEvents();
        QApplication::processEvents();
    }

    /// Only now: every dialog the teardown itself can raise -- the "save it?" prompt above, and the
    /// one a save that is refused raises -- has to be dismissed while it is up.
    if (m_dialogWatchdog) {
        m_dialogWatchdog->stop();
    }

    /// The refusal test leaves its page artifact read-only on purpose. Put it back so the store
    /// this run created can be removed.
    const QString artifact = artifactFor(0);
    if (QFileInfo::exists(artifact)) {
        QFile::setPermissions(artifact,
                              QFile::ReadOwner | QFile::WriteOwner
                                  | QFile::ReadUser | QFile::WriteUser);
    }
    QDir(projectsRoot()).removeRecursively();
}

bool PdfNavigatorIntegrationTest::useNotebook(const QString &name)
{
    const QString source = m_dir.filePath(name + QStringLiteral(".pdf"));
    if (!QFileInfo::exists(source) && !QFile::copy(m_fixture, source)) {
        qWarning("[nav-integration] cannot copy the fixture to %s", qPrintable(source));
        return false;
    }

    QString why;
    if (!navigator()->openNotebook(source, &why)) {
        qWarning("[nav-integration] openNotebook failed: %s", qPrintable(why));
        return false;
    }

    return navigator()->currentDocument() != nullptr;
}

void PdfNavigatorIntegrationTest::drawInk(KisDocument *document)
{
    QVERIFY(document);

    const KisImageSP image = document->image();
    QVERIFY(image);

    KisPaintLayer *layer = inkLayer(image);
    QVERIFY(layer);
    layer->paintDevice()->fill(InkMark, KoColor(QColor(0, 0, 0), image->colorSpace()));
    QVERIFY2(inkMarkPresent(image), "the mark did not land on the ink layer");

    /// The flag the page turn reads. A brush stroke arrives at the same flag through
    /// KisDocument::setImageModified(); the pixels are placed directly here so that what is under
    /// test is the page window and the save.
    document->setModified(true);
    QVERIFY(document->isModified());
}

QString PdfNavigatorIntegrationTest::artifactFor(int index) const
{
    return QDir(navigator()->projectDir()).filePath(PdfSession::pageFileName(index));
}

bool PdfNavigatorIntegrationTest::waitForInk(const QString &path, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (!PdfInkLoader::loadInk(path).isNull()) {
            return true;
        }
        QTest::qWait(50);
    }
    return !PdfInkLoader::loadInk(path).isNull();
}

/**
 * A page turn with nothing to protect: the page that leaves has no unsaved ink, so the window
 * drops it and nothing is written.
 */
void PdfNavigatorIntegrationTest::testCleanTurnEvictsWithoutSaving()
{
    QVERIFY(useNotebook(QStringLiteral("clean")));
    QCOMPARE(navigator()->currentIndex(), 0);
    QVERIFY(navigator()->pageCount() >= 2);
    QVERIFY(!navigator()->currentDocument()->isModified());
    QCOMPARE(navigator()->pageWindow().capacity(), navigator()->scope());

    QString why;
    QVERIFY2(navigator()->next(&why), qPrintable(why));
    QCOMPARE(navigator()->currentIndex(), 1);

    QCOMPARE(navigator()->pageWindow().evictionCount(), 1);
    QCOMPARE(navigator()->pageWindow().savedBeforeEvictionCount(), 0);
    QCOMPARE(navigator()->pageWindow().blockedEvictionCount(), 0);
    QCOMPARE(navigator()->pageWindow().openPages(), QList<int>{1});

    /// A clean page costs a render to reopen and never a write, so no artifact was made.
    QVERIFY(!QFileInfo::exists(artifactFor(0)));

    QVERIFY2(navigator()->previous(&why), qPrintable(why));
    QCOMPARE(navigator()->currentIndex(), 0);
    QCOMPARE(navigator()->pageWindow().evictionCount(), 2);
}

/**
 * A page turn that has to save first: the ink is on disk before the page is let go, and it comes
 * back with the page when the page is opened again.
 */
void PdfNavigatorIntegrationTest::testDirtyPageIsSavedBeforeEvictionAndTheInkComesBack()
{
    QVERIFY(useNotebook(QStringLiteral("dirty")));

    KisDocument *firstPage = navigator()->currentDocument();
    QVERIFY(firstPage);
    drawInk(firstPage);

    QString why;
    QVERIFY2(navigator()->next(&why), qPrintable(why));
    QCOMPARE(navigator()->currentIndex(), 1);

    /// The page could only be dropped because it was written first.
    QCOMPARE(navigator()->pageWindow().evictionCount(), 1);
    QCOMPARE(navigator()->pageWindow().savedBeforeEvictionCount(), 1);
    QCOMPARE(navigator()->pageWindow().blockedEvictionCount(), 0);

    /// A different document is open, and the one that left is really on disk.
    QVERIFY(navigator()->currentDocument() != firstPage);
    const QString artifact = artifactFor(0);
    QVERIFY2(waitForInk(artifact), qPrintable(artifact));

    /// And the ink comes back where it was drawn, which is the round trip the export rests on.
    QVERIFY2(navigator()->previous(&why), qPrintable(why));
    QCOMPARE(navigator()->currentIndex(), 0);
    QVERIFY2(inkMarkPresent(navigator()->currentDocument()->image()),
             "the ink saved before the eviction did not come back with the page");
}

/**
 * A page turn that cannot be completed. The save fails, so the eviction is refused, so the turn
 * is refused: the page that is open stays open, still holding an ink that is on disk nowhere,
 * and the destination is not half-written.
 */
void PdfNavigatorIntegrationTest::testRefusedSaveKeepsThePageOpenAndTheInkIntact()
{
    QVERIFY(useNotebook(QStringLiteral("refused")));

    KisDocument *page = navigator()->currentDocument();
    QVERIFY(page);
    drawInk(page);

    /// A destination that exists and cannot be written: Krita refuses such a save before it
    /// starts, which is exactly the refusal the window has to act on.
    const QString artifact = artifactFor(0);
    QVERIFY(QDir().mkpath(QFileInfo(artifact).absolutePath()));
    const QByteArray sentinel("this is not a saved page\n");
    {
        QFile file(artifact);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(sentinel), qint64(sentinel.size()));
        file.close();
    }
    QVERIFY(QFile::setPermissions(artifact, QFile::ReadOwner | QFile::ReadUser));
    QVERIFY(!QFileInfo(artifact).isWritable());

    QString why;
    QVERIFY2(!navigator()->next(&why), "the page turn was allowed although its save had failed");
    QVERIFY2(why.contains(QStringLiteral("saving it failed")), qPrintable(why));

    /// Nothing moved: same page, same document, still dirty, ink still on it.
    QCOMPARE(navigator()->currentIndex(), 0);
    QCOMPARE(navigator()->currentDocument(), page);
    QVERIFY(page->isModified());
    QVERIFY2(inkMarkPresent(page->image()), "the refused page turn lost the ink");

    /// And the unwritable destination was not touched.
    {
        QFile file(artifact);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), sentinel);
    }

    QCOMPARE(navigator()->pageWindow().blockedEvictionCount(), 1);
    QCOMPARE(navigator()->pageWindow().evictionCount(), 0);
    QCOMPARE(navigator()->pageWindow().savedBeforeEvictionCount(), 0);
    QCOMPARE(navigator()->pageWindow().openPages(), QList<int>{0});

    /// Refused again rather than once by accident, and the page is still the one that is open.
    QVERIFY(!navigator()->next(&why));
    QCOMPARE(navigator()->pageWindow().blockedEvictionCount(), 2);
    QCOMPARE(navigator()->currentIndex(), 0);
    QVERIFY2(inkMarkPresent(page->image()), "the second refusal lost the ink");

    /// The refusal was the destination and nothing else: once the destination can be written the
    /// same turn succeeds and the ink reaches the disk.
    QVERIFY(QFile::setPermissions(artifact,
                                  QFile::ReadOwner | QFile::WriteOwner
                                      | QFile::ReadUser | QFile::WriteUser));
    QVERIFY(QFileInfo(artifact).isWritable());

    QVERIFY2(navigator()->next(&why), qPrintable(why));
    QCOMPARE(navigator()->currentIndex(), 1);
    QVERIFY2(waitForInk(artifact), qPrintable(artifact));
    QCOMPARE(navigator()->pageWindow().evictionCount(), 1);
    QCOMPARE(navigator()->pageWindow().savedBeforeEvictionCount(), 1);
    QCOMPARE(navigator()->pageWindow().blockedEvictionCount(), 2);
}

int main(int argc, char *argv[])
{
    qputenv("LANGUAGE", "en");
    QStandardPaths::setTestModeEnabled(true);

    /// A safe assert in a headless run has nobody to press Ignore, and the dialog it would put up
    /// is answered by the watchdog below as if Abort had been pressed -- Krita aborts. This is the
    /// switch for exactly that: an ignorable assert warns and recovers, as it would for a user who
    /// pressed Ignore. See libs/global/kis_assert.cpp.
    qputenv("KRITA_NO_ASSERT_MSG", "1");

    /// The ink-only save is Krita's .kra export, which is a plugin: without it the save the test
    /// is about cannot even start. The build puts the plugins in one directory, which is what the
    /// plugin trader reads when the variable is set.
    qputenv("KRITA_PLUGIN_PATH", QByteArray(PDFIO_PLUGIN_DIR));
    qputenv("EXTRA_RESOURCE_DIRS", QByteArray(KRITA_RESOURCE_DIRS_FOR_TESTS));

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("PdfNavigatorIntegrationTest"));

    PdfNavigatorIntegrationTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "PdfNavigatorIntegrationTest.moc"
