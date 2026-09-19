/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfIoPlugin.h"
#include "PdfRendererSpike.h"

#include <kpluginfactory.h>

K_PLUGIN_FACTORY_WITH_JSON(PdfIoPluginFactory, "kritapdfio.json", registerPlugin<PdfIoPlugin>();)

PdfIoPlugin::PdfIoPlugin(QObject *parent, const QVariantList &)
    : KisActionPlugin(parent)
{
    /// Temporary: answers whether the Android render backend can be pure C++.
    PdfRendererSpike::run();
}

PdfIoPlugin::~PdfIoPlugin()
{
}

#include "PdfIoPlugin.moc"
