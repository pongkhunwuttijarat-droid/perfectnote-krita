/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfProjectBuilder.h"

#include "backend/PdfRenderBackend.h"

#include <QImage>

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

QString PdfProjectBuilder::backgroundLayerName()
{
    return QStringLiteral("PDF page");
}

QString PdfProjectBuilder::inkLayerName()
{
    return QStringLiteral("Ink");
}

QString PdfProjectBuilder::inkStrokeLayerName()
{
    return QStringLiteral("Layer 1");
}

KisNodeSP PdfProjectBuilder::inkStrokeLayer(const KisImageSP &image)
{
    if (!image || !image->root() || image->root()->childCount() < 2) {
        return KisNodeSP();
    }

    KisNodeSP group = image->root()->at(1);
    if (!group || group->childCount() == 0) {
        return KisNodeSP();
    }

    return group->at(0);
}

KisImageSP PdfProjectBuilder::buildPageImage(const PdfPageRecord &page,
                                             PdfRenderBackend &backend,
                                             qreal dpi,
                                             QString *why)
{
    if (!page.sizePt.isValid()) {
        fail(why, QStringLiteral("page %1 has no usable geometry").arg(page.index + 1));
        return KisImageSP();
    }

    const QImage rendered = backend.renderPage(page.index, dpi);
    if (rendered.isNull()) {
        fail(why, QStringLiteral("the renderer produced nothing for page %1").arg(page.index + 1));
        return KisImageSP();
    }

    const KoColorSpace *colorSpace = KoColorSpaceRegistry::instance()->rgb8();
    if (!colorSpace) {
        fail(why, QStringLiteral("no RGB color space is available"));
        return KisImageSP();
    }

    /// The image is measured in pixels of the render, so nothing downstream has to redo the
    /// point-to-pixel conversion that the renderer already made.
    KisImageSP image = new KisImage(0, rendered.width(), rendered.height(), colorSpace,
                                    QStringLiteral("PDF page %1").arg(page.index + 1));
    image->setResolution(dpi, dpi);

    KisPaintLayerSP background = new KisPaintLayer(image, backgroundLayerName(), OPACITY_OPAQUE_U8);
    background->paintDevice()->convertFromQImage(rendered, 0, 0, 0);

    /// The page artwork is not ours to edit; only the Ink group is written by the session.
    background->setUserLocked(true);

    KisGroupLayerSP ink = new KisGroupLayer(image, inkLayerName(), OPACITY_OPAQUE_U8, colorSpace);

    /// Added in order: the background first, so the Ink group ends up above it.
    image->addNode(background, image->root());
    image->addNode(ink, image->root());

    /// The Ink group needs a paint layer of its own. A group is not paintable, so without this
    /// the user selects Ink, draws, and nothing happens at all.
    KisPaintLayerSP stroke = new KisPaintLayer(image, inkStrokeLayerName(), OPACITY_OPAQUE_U8);
    image->addNode(stroke, ink);

    return image;
}
