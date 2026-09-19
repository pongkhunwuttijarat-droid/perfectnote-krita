/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfStripBuilder.h"

#include "backend/PdfRenderBackend.h"
#include "session/PdfInkLoader.h"

#include <QDir>

#include <kis_group_layer.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>

#include <KoColorSpaceConstants.h>
#include <KoColorSpaceRegistry.h>

namespace {

void fail(QString *why, const QString &message)
{
    if (why) {
        *why = message;
    }
}

} // namespace

QString PdfStripBuilder::inkGroupName(int page)
{
    return QStringLiteral("Ink %1").arg(page + 1);
}

QString PdfStripBuilder::backgroundLayerName(int page)
{
    return QStringLiteral("PDF page %1").arg(page + 1);
}

QString PdfStripBuilder::inkLayerName(int page)
{
    return QStringLiteral("Layer 1");
}

PdfStripBuilder::Strip PdfStripBuilder::build(const PdfSessionManifest &manifest,
                                              int activePage,
                                              int scope,
                                              qreal dpi,
                                              PdfRenderBackend &backend,
                                              const QString &projectDir,
                                              QString *why)
{
    Strip strip;

    const PdfStripLayout layout = PdfStripLayout::forWindow(manifest, activePage, scope, dpi);
    if (!layout.isValid()) {
        fail(why, QStringLiteral("the strip has no valid layout"));
        return strip;
    }
    if (!backend.isOpen()) {
        fail(why, QStringLiteral("the renderer is not open"));
        return strip;
    }

    const KoColorSpace *colorSpace = KoColorSpaceRegistry::instance()->rgb8();
    if (!colorSpace) {
        fail(why, QStringLiteral("no RGB color space is available"));
        return strip;
    }

    /// The image is the layout's size and stays that size, which is what lets the window roll
    /// without rebuilding anything.
    strip.image = new KisImage(0,
                               layout.imageSize().width(),
                               layout.imageSize().height(),
                               colorSpace,
                               QStringLiteral("notebook"));
    strip.image->setResolution(dpi, dpi);
    strip.layout = layout;

    const QDir project(projectDir);
    const QList<PdfStripLayout::Slot> slots = layout.slots();

    for (int i = 0; i < slots.size(); ++i) {
        const PdfStripLayout::Slot &slot = slots.at(i);
        if (slot.page < 0) {
            continue;
        }

        const bool active = (i == layout.activeSlot());

        /// The page itself, as a locked layer. Drawn at the slot's own place in the strip.
        const QImage rendered = backend.renderPage(slot.page, dpi);
        KisPaintLayerSP background =
            new KisPaintLayer(strip.image, backgroundLayerName(slot.page), OPACITY_OPAQUE_U8);
        if (!rendered.isNull()) {
            background->paintDevice()->convertFromQImage(rendered, nullptr,
                                                         slot.rect.x(), slot.rect.y());
        }
        background->setUserLocked(true);

        /// The ink of this page: restored if it has been drawn on before, blank if not.
        KisGroupLayerSP ink =
            new KisGroupLayer(strip.image, inkGroupName(slot.page), OPACITY_OPAQUE_U8, colorSpace);
        KisPaintLayerSP stroke = new KisPaintLayer(strip.image, inkLayerName(slot.page),
                                                   OPACITY_OPAQUE_U8);

        const QImage savedInk =
            PdfInkLoader::loadInk(project.filePath(manifest.pages.at(slot.page).kraFile), nullptr);
        if (!savedInk.isNull()) {
            stroke->paintDevice()->convertFromQImage(savedInk, nullptr,
                                                     slot.rect.x(), slot.rect.y());
        }

        /// Locked together, group and layer, so that a stroke aimed at a neighbouring page is
        /// refused by Krita rather than quietly landing somewhere it does not belong.
        ink->setUserLocked(!active);
        stroke->setUserLocked(!active);

        strip.image->addNode(background, strip.image->root());
        strip.image->addNode(ink, strip.image->root());
        strip.image->addNode(stroke, ink);

        if (active) {
            strip.activeInkLayer = stroke;
        }
    }

    if (!strip.activeInkLayer) {
        fail(why, QStringLiteral("the strip has no paintable slot"));
    }

    return strip;
}
