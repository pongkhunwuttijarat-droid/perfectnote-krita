/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef POPPLERRENDERBACKEND_H
#define POPPLERRENDERBACKEND_H

#include "backend/PdfRenderBackend.h"

#include <memory>

#include <poppler-qt6.h>

class PopplerRenderBackend : public PdfRenderBackend
{
public:
    PopplerRenderBackend();
    ~PopplerRenderBackend() override;

    bool open(const QString &path) override;
    bool isOpen() const override;
    int pageCount() const override;
    PdfPageInfo pageInfo(int index) const override;
    QImage renderPage(int index, qreal dpi) const override;

private:
    std::unique_ptr<Poppler::Document> m_document;
};

#endif // POPPLERRENDERBACKEND_H
