/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "backend/PdfRenderBackend.h"

#if defined(PDFIO_HAVE_POPPLER)
#include "backends/poppler/PopplerRenderBackend.h"
#elif defined(Q_OS_ANDROID)
#include "backends/android/AndroidRenderBackend.h"
#endif

PdfRenderBackend *PdfRenderBackend::create()
{
#if defined(PDFIO_HAVE_POPPLER)
    return new PopplerRenderBackend();
#elif defined(Q_OS_ANDROID)
    return new AndroidRenderBackend();
#else
    return nullptr;
#endif
}
