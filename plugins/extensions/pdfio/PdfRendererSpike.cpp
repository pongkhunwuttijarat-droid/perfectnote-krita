/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfRendererSpike.h"

#include <QDebug>

#ifdef Q_OS_ANDROID

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QJniEnvironment>
#include <QJniObject>
#else
#include <QAndroidJniEnvironment>
#include <QAndroidJniObject>
using QJniEnvironment = QAndroidJniEnvironment;
using QJniObject = QAndroidJniObject;
#endif

namespace {

const char *TAG = "[pdfspike]";

/// One page, 200x200 pt, black square in the bottom-left corner of the media box.
QByteArray minimalPdf()
{
    const QByteArray content = "0 0 0 rg\n0 0 40 40 re f\n";
    QByteArray objects[4];
    objects[0] = "<< /Type /Catalog /Pages 2 0 R >>";
    objects[1] = "<< /Type /Pages /Kids [3 0 R] /Count 1 >>";
    objects[2] = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                 "/Contents 4 0 R /Resources << >> >>";
    objects[3] = "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n"
                 + content + "endstream";

    QByteArray out = "%PDF-1.4\n%\xe2\xe3\xcf\xd3\n";
    QVector<int> offsets;
    for (int i = 0; i < 4; ++i) {
        offsets << out.size();
        out += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const int xrefAt = out.size();
    out += "xref\n0 5\n0000000000 65535 f \n";
    for (int offset : offsets) {
        out += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    }
    out += "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n"
           + QByteArray::number(xrefAt) + "\n%%EOF\n";
    return out;
}

void reportJniException(const char *where)
{
    QJniEnvironment env;
    if (!env->ExceptionCheck()) {
        return;
    }

    /// Read the message before clearing, using plain JNI: a pending exception makes the
    /// NextJni wrappers unreliable, and the message is the whole point of the probe.
    jthrowable throwable = env->ExceptionOccurred();
    env->ExceptionClear();

    QString message = QStringLiteral("(no message)");
    if (throwable) {
        jclass cls = env->GetObjectClass(throwable);
        jmethodID toString = env->GetMethodID(cls, "toString", "()Ljava/lang/String;");
        if (toString) {
            jstring text = jstring(env->CallObjectMethod(throwable, toString));
            if (text) {
                const char *utf = env->GetStringUTFChars(text, nullptr);
                if (utf) {
                    message = QString::fromUtf8(utf);
                    env->ReleaseStringUTFChars(text, utf);
                }
                env->DeleteLocalRef(text);
            }
        }
        env->DeleteLocalRef(cls);
        env->DeleteLocalRef(throwable);
    }

    qWarning() << TAG << "JNI exception at" << where << ":" << message;
}

quint32 pixelAt(const QJniObject &bitmap, int x, int y)
{
    const jint pixel = bitmap.callMethod<jint>("getPixel", "(II)I", jint(x), jint(y));
    return quint32(pixel);
}

} // namespace

namespace PdfRendererSpike {

void run()
{
    const QString path =
        QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
            .filePath(QStringLiteral("pdfio-spike.pdf"));

    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            qWarning() << TAG << "cannot write" << path;
            return;
        }
        file.write(minimalPdf());
    }
    qDebug() << TAG << "wrote" << path << QFileInfo(path).size() << "bytes";

    QJniObject jpath = QJniObject::fromString(path);
    QJniObject jfile("java/io/File", "(Ljava/lang/String;)V", jpath.object<jstring>());
    if (!jfile.isValid()) {
        qWarning() << TAG << "java.io.File construction failed";
        return;
    }

    /// ParcelFileDescriptor.MODE_READ_ONLY
    const jint modeReadOnly = jint(0x10000000);
    QJniObject descriptor =
        QJniObject::callStaticObjectMethod("android/os/ParcelFileDescriptor", "open",
                                           "(Ljava/io/File;I)Landroid/os/ParcelFileDescriptor;",
                                           jfile.object(), modeReadOnly);
    if (!descriptor.isValid()) {
        reportJniException("ParcelFileDescriptor.open");
        qWarning() << TAG << "ParcelFileDescriptor.open failed";
        return;
    }

    QJniObject renderer("android/graphics/pdf/PdfRenderer",
                        "(Landroid/os/ParcelFileDescriptor;)V", descriptor.object());
    if (!renderer.isValid()) {
        reportJniException("new PdfRenderer");
        qWarning() << TAG << "PdfRenderer construction failed";
        return;
    }

    const jint pageCount = renderer.callMethod<jint>("getPageCount", "()I");
    QJniObject page = renderer.callObjectMethod(
        "openPage", "(I)Landroid/graphics/pdf/PdfRenderer$Page;", jint(0));
    if (!page.isValid()) {
        reportJniException("PdfRenderer.openPage");
        qWarning() << TAG << "openPage failed";
        return;
    }

    const jint width = page.callMethod<jint>("getWidth", "()I");
    const jint height = page.callMethod<jint>("getHeight", "()I");
    qDebug() << TAG << "pageCount" << pageCount << "page0" << width << "x" << height;

    QJniObject config = QJniObject::getStaticObjectField(
        "android/graphics/Bitmap$Config", "ARGB_8888", "Landroid/graphics/Bitmap$Config;");
    QJniObject bitmap = QJniObject::callStaticObjectMethod(
        "android/graphics/Bitmap", "createBitmap",
        "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;",
        jint(width), jint(height), config.object());
    if (!bitmap.isValid()) {
        reportJniException("Bitmap.createBitmap");
        qWarning() << TAG << "createBitmap failed";
        return;
    }
    qDebug() << TAG << "config valid" << config.isValid()
             << "bitmap valid" << bitmap.isValid()
             << "mutable" << bitmap.callMethod<jboolean>("isMutable", "()Z");

    bitmap.callMethod<void>("eraseColor", "(I)V", jint(0xffffffffu));

    /// PdfRenderer.Page.RENDER_MODE_FOR_DISPLAY is 1, not 0: mode 0 is rejected with
    /// "Unsupported render mode". RENDER_MODE_FOR_PRINT is 2.
    const jint renderModeForDisplay = jint(1);

    QJniObject noObject;
    page.callMethod<void>("render",
                          "(Landroid/graphics/Bitmap;Landroid/graphics/Rect;"
                          "Landroid/graphics/Matrix;I)V",
                          bitmap.object(), noObject.object(), noObject.object(),
                          renderModeForDisplay);
    reportJniException("Page.render");

    const int x[2] = { width / 10, width - width / 10 };
    const int y[2] = { height / 10, height - height / 10 };
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 2; ++col) {
            qDebug() << TAG << "sample"
                     << (row ? "bottom" : "top") << (col ? "right" : "left")
                     << QString::number(pixelAt(bitmap, x[col], y[row]), 16);
        }
    }

    page.callMethod<void>("close", "()V");
    renderer.callMethod<void>("close", "()V");
}

} // namespace PdfRendererSpike

#else

namespace PdfRendererSpike {
void run()
{
    /// Desktop has Poppler; the probe is only about the Android backend.
}
} // namespace PdfRendererSpike

#endif // Q_OS_ANDROID
