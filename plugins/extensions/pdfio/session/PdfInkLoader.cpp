/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfInkLoader.h"

#include <QDebug>
#include <QFileInfo>

#include <KArchiveDirectory>
#include <KArchiveEntry>
#include <KArchiveFile>
#include <KZip>

namespace {

void fail(QString *why, const QString &message)
{
    if (why) {
        *why = message;
    }
}

/// A paint layer is stored as a PNG with no extension, next to companions that carry the same
/// name plus a suffix: the default pixel value, the colour profile, a pixel selection. Only the
/// PNG itself is wanted, and it is the only one that is actually an image.
bool isPaintLayer(const QString &name)
{
    if (!name.contains(QLatin1String("/layers/"))) {
        return false;
    }
    return !name.endsWith(QLatin1String(".defaultpixel"))
        && !name.endsWith(QLatin1String(".icc"))
        && !name.endsWith(QLatin1String(".pixelselection"))
        && !name.endsWith(QLatin1String(".pixelselection.defaultpixel"));
}

void collectLayers(const KArchiveDirectory *directory, const QString &prefix, QList<QByteArray> *found)
{
    const QStringList entries = directory->entries();
    for (const QString &entry : entries) {
        const KArchiveEntry *child = directory->entry(entry);
        if (!child) {
            continue;
        }

        const QString path = prefix + QLatin1Char('/') + entry;
        if (const KArchiveDirectory *sub = dynamic_cast<const KArchiveDirectory *>(child)) {
            collectLayers(sub, path, found);
            continue;
        }

        if (const KArchiveFile *file = dynamic_cast<const KArchiveFile *>(child)) {
            if (isPaintLayer(path)) {
                found->append(file->data());
            }
        }
    }
}

} // namespace

QImage PdfInkLoader::loadInk(const QString &kraPath, QString *why)
{
    if (!QFileInfo::exists(kraPath)) {
        /// Not an error: a page that was never drawn on has no artifact.
        return QImage();
    }

    KZip zip(kraPath);
    if (!zip.open(QIODevice::ReadOnly)) {
        fail(why, QStringLiteral("cannot read %1").arg(kraPath));
        return QImage();
    }

    const KArchiveDirectory *root = zip.directory();
    if (!root) {
        fail(why, QStringLiteral("%1 has no directory").arg(kraPath));
        return QImage();
    }

    QList<QByteArray> layers;
    collectLayers(root, QString(), &layers);

    /// The biggest one: a note is made of paint, and any companion entry is small.
    QByteArray best;
    for (const QByteArray &candidate : layers) {
        if (candidate.size() > best.size()) {
            best = candidate;
        }
    }

    if (best.isEmpty()) {
        return QImage();
    }

    QImage ink;
    if (!ink.loadFromData(best, "PNG")) {
        fail(why, QStringLiteral("the ink layer of %1 is not readable").arg(kraPath));
        return QImage();
    }

    return ink;
}
