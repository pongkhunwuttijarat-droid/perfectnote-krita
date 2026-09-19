/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFIOPLUGIN_H
#define PDFIOPLUGIN_H

#include <KisActionPlugin.h>

/**
 * Entry point of the pdfio extension: PDF pages as locked backgrounds with an ink group
 * on top, plus a materialised note project around them.
 *
 * The plugin is deliberately not gated with if(ANDROID) the way the pen extensions are:
 * desktop is the only platform where the whole session, geometry and export logic can be
 * tested, so it has to keep building there.
 */
class PdfIoPlugin : public KisActionPlugin
{
    Q_OBJECT
public:
    PdfIoPlugin(QObject *parent, const QVariantList &);
    ~PdfIoPlugin() override;
};

#endif // PDFIOPLUGIN_H
