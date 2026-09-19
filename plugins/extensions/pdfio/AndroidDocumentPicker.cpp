/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "AndroidDocumentPicker.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#if defined(Q_OS_ANDROID)

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QJniEnvironment>
#include <QJniObject>
#else
#include <QAndroidActivityResultReceiver>
#include <QAndroidJniEnvironment>
#include <QAndroidJniObject>
#include <QtAndroid>
using QJniEnvironment = QAndroidJniEnvironment;
using QJniObject = QAndroidJniObject;
#endif

namespace {

const int PickRequestCode = 7001;

void reportJniException(const char *where)
{
    QJniEnvironment env;
    if (!env->ExceptionCheck()) {
        return;
    }
    env->ExceptionDescribe();
    env->ExceptionClear();
    qWarning() << "[pdfio] JNI exception at" << where;
}

/// Copies what a content:// URI offers into a real file, and returns that path.
QString copyContentToCache(const QString &uri)
{
    QJniObject activity = QJniObject::callStaticObjectMethod("org/qtproject/qt5/android/QtNative",
                                                             "activity",
                                                             "()Landroid/app/Activity;");
    if (!activity.isValid()) {
        return QString();
    }

    QJniObject contentResolver = activity.callObjectMethod("getContentResolver",
                                                           "()Landroid/content/ContentResolver;");
    QJniObject juri = QJniObject::callStaticObjectMethod("android/net/Uri", "parse",
                                                         "(Ljava/lang/String;)Landroid/net/Uri;",
                                                         QJniObject::fromString(uri).object<jstring>());
    if (!contentResolver.isValid() || !juri.isValid()) {
        return QString();
    }

    /// openInputStream can throw FileNotFoundException, and a pending exception makes every
    /// later JNI call unreliable, so it is cleared immediately.
    QJniObject stream = contentResolver.callObjectMethod(
        "openInputStream", "(Landroid/net/Uri;)Ljava/io/InputStream;", juri.object());
    reportJniException("ContentResolver.openInputStream");
    if (!stream.isValid()) {
        return QString();
    }

    const QString target =
        QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
            .filePath(QStringLiteral("pdfio-picked.pdf"));
    QFile file(target);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        stream.callMethod<void>("close", "()V");
        return QString();
    }

    /// Read the stream through the framework rather than inventing a JNI loop: a small Java
    /// helper on the other side of the bridge does the copying.
    QJniEnvironment env;
    jbyteArray buffer = env->NewByteArray(64 * 1024);
    if (!buffer) {
        file.close();
        stream.callMethod<void>("close", "()V");
        return QString();
    }

    while (true) {
        const jint read = stream.callMethod<jint>("read", "([B)I", buffer);
        if (read <= 0) {
            break;
        }
        QByteArray chunk(int(read), Qt::Uninitialized);
        env->GetByteArrayRegion(buffer, 0, read, reinterpret_cast<jbyte *>(chunk.data()));
        file.write(chunk);
    }

    env->DeleteLocalRef(buffer);
    stream.callMethod<void>("close", "()V");
    file.close();

    return file.size() > 0 ? target : QString();
}

} // namespace

struct AndroidDocumentPicker::Private
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
{
};
#else
    : public QAndroidActivityResultReceiver
{
    AndroidDocumentPicker *q = nullptr;
    std::function<void(const QString &, const QString &)> callback;

    void handleActivityResult(int receiverRequestCode, int resultCode, const QAndroidJniObject &data) override
    {
        if (receiverRequestCode != PickRequestCode || !callback) {
            return;
        }

        auto done = callback;
        callback = nullptr;

        qWarning("[pdfio] picker returned: request %d result %d data %d",
                 receiverRequestCode, resultCode, int(data.isValid()));

        /// Activity.RESULT_OK is -1.
        if (resultCode != -1 || !data.isValid()) {
            done(QString(), QStringLiteral("no file was chosen"));
            return;
        }

        QJniObject uri = data.callObjectMethod("getData", "()Landroid/net/Uri;");
        if (!uri.isValid()) {
            done(QString(), QStringLiteral("the chosen file has no URI"));
            return;
        }

        QJniObject text = uri.callObjectMethod("toString", "()Ljava/lang/String;");
        qWarning("[pdfio] picked uri: %s", qPrintable(text.toString()));

        const QString local = copyContentToCache(text.toString());
        qWarning("[pdfio] copied to %s (%lld bytes)", qPrintable(local),
                 qint64(local.isEmpty() ? 0 : QFileInfo(local).size()));
        if (local.isEmpty()) {
            done(QString(), QStringLiteral("the chosen file could not be copied"));
            return;
        }

        done(local, QString());
    }
};
#endif

AndroidDocumentPicker::AndroidDocumentPicker(QObject *parent)
    : QObject(parent)
    , d(new Private)
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    d->q = this;
#endif
}

AndroidDocumentPicker::~AndroidDocumentPicker()
{
    delete d;
}

void AndroidDocumentPicker::pickPdf(std::function<void(const QString &, const QString &)> onPicked)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    Q_UNUSED(onPicked);
    onPicked(QString(), QStringLiteral("the document picker is only wired for the Qt5 Android build"));
#else
    d->callback = onPicked;

    QJniObject action = QJniObject::fromString(QStringLiteral("android.intent.action.OPEN_DOCUMENT"));
    QJniObject type = QJniObject::fromString(QStringLiteral("application/pdf"));

    QJniObject intent("android/content/Intent", "(Ljava/lang/String;)V", action.object<jstring>());
    intent.callObjectMethod("setType", "(Ljava/lang/String;)Landroid/content/Intent;", type.object<jstring>());
    intent.callObjectMethod("addCategory", "(Ljava/lang/String;)Landroid/content/Intent;",
                            QJniObject::fromString(QStringLiteral("android.intent.category.OPENABLE")).object<jstring>());

    /// Qt hands the result back to the receiver, so no Java of ours is needed to receive it.
    qWarning("[pdfio] launching the document picker");
    QtAndroid::startActivity(intent, PickRequestCode, d);
    reportJniException("startActivity(OPEN_DOCUMENT)");
#endif
}

#else

struct AndroidDocumentPicker::Private
{
};

AndroidDocumentPicker::AndroidDocumentPicker(QObject *parent)
    : QObject(parent)
    , d(new Private)
{
}

AndroidDocumentPicker::~AndroidDocumentPicker()
{
    delete d;
}

void AndroidDocumentPicker::pickPdf(std::function<void(const QString &, const QString &)> onPicked)
{
    onPicked(QString(), QStringLiteral("no document picker on this platform"));
}

#endif // Q_OS_ANDROID
