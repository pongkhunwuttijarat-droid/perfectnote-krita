/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFSESSIONMANIFEST_H
#define PDFSESSIONMANIFEST_H

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QSizeF>
#include <QString>

/**
 * One page of a note project.
 *
 * A record describes where the page sits in the PDF and which ink file belongs to it; it
 * never holds the page raster, which is derivable from the bundled source and is therefore
 * not persisted. That decision is what keeps a project proportional to its ink.
 */
struct PdfPageRecord {
    int index = -1;
    /// Displayed size in points, /Rotate already applied.
    QSizeF sizePt;
    /// The /Rotate the file declares, kept so the manifest survives a renderer change.
    int rotation = 0;
    /// Paths are relative to the project directory, so a project can be moved.
    QString kraFile;
    QString thumbFile;
    /// Bumped on every committed save of this page; used to reason about recovery.
    int generation = 0;
};

/**
 * manifest.json: the single durable description of a note project.
 *
 * The source PDF is immutable and never written back to, so the manifest only has to record
 * the identity of the source it was built from, the page list, and the ink that has since
 * been committed on top.
 */
class PdfSessionManifest
{
public:
    /// Bumped whenever the on-disk shape changes in a way older readers cannot handle.
    static const int CurrentSchema;

    int schema = 1;
    /// File name of the source inside the project directory, not a full path.
    QString sourceFile;
    /// Hex encoded SHA-256 of the source, so a moved or edited source is detected.
    QByteArray sourceSha256;
    qint64 sourceByteSize = 0;
    QList<PdfPageRecord> pages;

    bool isValid(QString *why = nullptr) const;

    QJsonObject toJson() const;
    static PdfSessionManifest fromJson(const QJsonObject &object, QString *why = nullptr);

    bool writeTo(const QString &path, QString *why = nullptr) const;
    static PdfSessionManifest readFrom(const QString &path, QString *why = nullptr);

    static QByteArray sha256OfFile(const QString &path);
};

#endif // PDFSESSIONMANIFEST_H
