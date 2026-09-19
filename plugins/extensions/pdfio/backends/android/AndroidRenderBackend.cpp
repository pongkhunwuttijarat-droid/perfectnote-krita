/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "AndroidRenderBackend.h"

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QJniEnvironment>
#else
#include <QAndroidJniEnvironment>
using QJniEnvironment = QAndroidJniEnvironment;
#endif

#include <QDebug>

namespace {

/// ParcelFileDescriptor.MODE_READ_ONLY
const jint ModeReadOnly = jint(0x10000000);

/// PdfRenderer.Page.RENDER_MODE_FOR_DISPLAY. Mode 0 is rejected with
/// "Unsupported render mode", which is worth remembering: the constants are 1 and 2.
const jint RenderModeForDisplay = jint(1);

void reportJniException(const char *where)
{
    QJniEnvironment env;
    if (!env->ExceptionCheck()) {
        return;
    }

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

    qWarning() << "[pdfio] JNI exception at" << where << ":" << message;
}

} // namespace

AndroidRenderBackend::AndroidRenderBackend() = default;

AndroidRenderBackend::~AndroidRenderBackend()
{
    if (m_renderer.isValid()) {
        m_renderer.callMethod<void>("close", "()V");
    }
    /// The descriptor is closed by the renderer, which took ownership of it.
}

bool AndroidRenderBackend::open(const QString &path)
{
    if (m_renderer.isValid()) {
        m_renderer.callMethod<void>("close", "()V");
        m_renderer = QJniObject();
    }
    m_pages.clear();

    QJniObject jpath = QJniObject::fromString(path);
    QJniObject file("java/io/File", "(Ljava/lang/String;)V", jpath.object<jstring>());
    if (!file.isValid()) {
        qWarning() << "[pdfio] cannot construct java.io.File for" << path;
        return false;
    }

    m_descriptor = QJniObject::callStaticObjectMethod(
        "android/os/ParcelFileDescriptor", "open",
        "(Ljava/io/File;I)Landroid/os/ParcelFileDescriptor;", file.object(), ModeReadOnly);
    if (!m_descriptor.isValid()) {
        reportJniException("ParcelFileDescriptor.open");
        return false;
    }

    m_renderer = QJniObject("android/graphics/pdf/PdfRenderer",
                            "(Landroid/os/ParcelFileDescriptor;)V", m_descriptor.object());
    if (!m_renderer.isValid()) {
        reportJniException("new PdfRenderer");
        m_descriptor = QJniObject();
        return false;
    }

    const jint count = m_renderer.callMethod<jint>("getPageCount", "()I");

    /// The geometry is collected once, here: PdfRenderer allows a single page to be open at a
    /// time, so walking them now keeps rendering free of that constraint.
    for (int i = 0; i < count; ++i) {
        QJniObject page = m_renderer.callObjectMethod(
            "openPage", "(I)Landroid/graphics/pdf/PdfRenderer$Page;", jint(i));
        if (!page.isValid()) {
            reportJniException("PdfRenderer.openPage");
            continue;
        }

        PdfPageInfo info;
        info.index = i;
        /// PdfRenderer reports the page at 72 dpi, which is the same unit the manifest uses.
        info.sizePt = QSizeF(page.callMethod<jint>("getWidth", "()I"),
                             page.callMethod<jint>("getHeight", "()I"));
        /// It exposes no /Rotate: what it does expose is already the displayed size, which is
        /// what the page transform is built from.
        info.rotation = 0;
        m_pages.append(info);

        page.callMethod<void>("close", "()V");
    }

    return true;
}

bool AndroidRenderBackend::isOpen() const
{
    return m_renderer.isValid();
}

int AndroidRenderBackend::pageCount() const
{
    return m_pages.size();
}

PdfPageInfo AndroidRenderBackend::pageInfo(int index) const
{
    if (index < 0 || index >= m_pages.size()) {
        return PdfPageInfo();
    }
    return m_pages.at(index);
}

QImage AndroidRenderBackend::renderPage(int index, qreal dpi) const
{
    if (!m_renderer.isValid() || index < 0 || index >= m_pages.size()) {
        return QImage();
    }

    const PdfPageInfo info = m_pages.at(index);
    const int width = qMax(1, qRound(info.sizePt.width() * dpi / 72.0));
    const int height = qMax(1, qRound(info.sizePt.height() * dpi / 72.0));

    QJniObject page = m_renderer.callObjectMethod(
        "openPage", "(I)Landroid/graphics/pdf/PdfRenderer$Page;", jint(index));
    if (!page.isValid()) {
        reportJniException("PdfRenderer.openPage");
        return QImage();
    }

    QJniObject config = QJniObject::getStaticObjectField(
        "android/graphics/Bitmap$Config", "ARGB_8888", "Landroid/graphics/Bitmap$Config;");
    QJniObject bitmap = QJniObject::callStaticObjectMethod(
        "android/graphics/Bitmap", "createBitmap",
        "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;",
        jint(width), jint(height), config.object());
    if (!bitmap.isValid()) {
        reportJniException("Bitmap.createBitmap");
        page.callMethod<void>("close", "()V");
        return QImage();
    }

    /// Filled with white first: the page raster is opaque, and anything the renderer leaves
    /// untouched would otherwise come through as transparent and be composited as black.
    bitmap.callMethod<void>("eraseColor", "(I)V", jint(0xffffffffu));

    QJniObject noObject;
    page.callMethod<void>("render",
                          "(Landroid/graphics/Bitmap;Landroid/graphics/Rect;"
                          "Landroid/graphics/Matrix;I)V",
                          bitmap.object(), noObject.object(), noObject.object(),
                          RenderModeForDisplay);
    reportJniException("Page.render");
    page.callMethod<void>("close", "()V");

    /// ARGB_8888 bitmaps are premultiplied, so the matching QImage format is the premultiplied
    /// one; w * 4 bytes per row is already aligned, so a single copy fits.
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);

    QJniEnvironment env;
    jintArray pixels = env->NewIntArray(width * height);
    if (!pixels) {
        return QImage();
    }
    bitmap.callMethod<void>("getPixels", "([IIIIIII)V", pixels, jint(0), jint(width),
                            jint(0), jint(0), jint(width), jint(height));
    reportJniException("Bitmap.getPixels");
    env->GetIntArrayRegion(pixels, 0, width * height, reinterpret_cast<jint *>(image.bits()));
    env->DeleteLocalRef(pixels);

    return image;
}

QString AndroidRenderBackend::pageText(int index) const
{
    Q_UNUSED(index);
    return QString();
}
