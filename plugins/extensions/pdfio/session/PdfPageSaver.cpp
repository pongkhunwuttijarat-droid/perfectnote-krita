/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfPageSaver.h"

#include <KisDocument.h>
#include <KisPart.h>

#include <kis_group_layer.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>

namespace {

void fail(QString *why, const QString &message)
{
    if (why) {
        *why = message;
    }
}

/**
 * Copies the paintable children of the ink group into \a target.
 *
 * Layer name, opacity and pixels are carried over; masks and effects are not, which is a
 * deliberate first cut: the ink a note is made of is paint, and anything Krita-specific that
 * cannot be reproduced is better rejected loudly later than flattened silently now.
 */
void copyInkLayers(KisImageSP target, KisNodeSP inkGroup, KisNodeSP parent)
{
    for (quint32 i = 0; i < inkGroup->childCount(); ++i) {
        KisNodeSP child = inkGroup->at(i);
        KisPaintLayer *paint = qobject_cast<KisPaintLayer *>(child.data());
        if (!paint) {
            continue;
        }

        KisPaintLayerSP copy = new KisPaintLayer(target, paint->name(), paint->opacity());
        copy->paintDevice()->makeCloneFrom(paint->paintDevice(), paint->paintDevice()->extent());
        target->addNode(copy, parent);
    }
}

} // namespace

KisDocument *PdfPageSaver::createInkOnlyDocument(const KisImageSP &source, QString *why)
{
    if (!source) {
        fail(why, QStringLiteral("no page image to save"));
        return nullptr;
    }

    if (!source->root() || source->root()->childCount() < 2) {
        fail(why, QStringLiteral("the page has no Ink group"));
        return nullptr;
    }

    KisNodeSP inkGroup = source->root()->at(1);
    if (!inkGroup || !qobject_cast<KisGroupLayer *>(inkGroup.data())) {
        fail(why, QStringLiteral("the second layer of the page is not the Ink group"));
        return nullptr;
    }

    KisDocument *document = KisPart::instance()->createDocument();

    KisImageSP inkOnly = new KisImage(document->createUndoStore(),
                                      source->width(), source->height(), source->colorSpace(),
                                      QStringLiteral("ink"));
    inkOnly->setResolution(source->xRes(), source->yRes());

    copyInkLayers(inkOnly, inkGroup, inkOnly->rootLayer());
    document->setCurrentImage(inkOnly, false);

    return document;
}

bool PdfPageSaver::saveInkOnly(KisDocument *inkOnlyDocument, const QString &path, QString *why)
{
    if (!inkOnlyDocument) {
        fail(why, QStringLiteral("no document to save"));
        return false;
    }

    if (!inkOnlyDocument->saveAs(path, QByteArrayLiteral("application/x-krita"), false)) {
        fail(why, QStringLiteral("could not start saving %1").arg(path));
        return false;
    }

    return true;
}
