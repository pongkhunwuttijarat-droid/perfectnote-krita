/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ANDROIDRENDERBACKEND_H
#define ANDROIDRENDERBACKEND_H

#include "backend/PdfRenderBackend.h"

#include <QList>

/// Qt5 keeps these in AndroidExtras as QAndroidJni*, Qt6 moved them into QtCore under the new
/// names. The same compatibility shim Krita itself uses in libs/global/KisAndroidUtils.cpp.
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QJniObject>
#else
#include <QAndroidJniObject>
using QJniObject = QAndroidJniObject;
#endif

/**
 * Renders PDF pages with android.graphics.pdf.PdfRenderer.
 *
 * Android has no Poppler, but it does have a renderer in the framework, and a framework class can
 * be reached straight from C++ through QJniObject. That is what keeps this backend inside the
 * plugin: no Java of ours is added to the APK, and nothing of the vendor SDK is involved.
 */
class AndroidRenderBackend : public PdfRenderBackend
{
public:
    AndroidRenderBackend();
    ~AndroidRenderBackend() override;

    bool open(const QString &path) override;
    bool isOpen() const override;
    int pageCount() const override;
    PdfPageInfo pageInfo(int index) const override;
    QImage renderPage(int index, qreal dpi) const override;

    /// PdfRenderer extracts no text, so an export from here cannot check the source that way.
    QString pageText(int index) const override;

private:
    QJniObject m_renderer;
    QJniObject m_descriptor;
    QList<PdfPageInfo> m_pages;
};

#endif // ANDROIDRENDERBACKEND_H
