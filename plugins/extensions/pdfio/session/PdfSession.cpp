/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfSession.h"

#include "backend/PdfRenderBackend.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace {

QString numbered(const QString &directory, const QString &prefix, int index, const QString &suffix)
{
    return QStringLiteral("%1/p%2%3").arg(directory,
                                          QString::number(index + 1).rightJustified(4, QLatin1Char('0')),
                                          suffix);
}

void fail(QString *why, const QString &message)
{
    if (why) {
        *why = message;
    }
}

} // namespace

QString PdfSession::manifestPath(const QString &projectDir)
{
    return QDir(projectDir).filePath(QStringLiteral("manifest.json"));
}

QString PdfSession::sourcePath(const QString &projectDir, const QString &sourceFile)
{
    return QDir(projectDir).filePath(sourceFile);
}

QString PdfSession::pageFileName(int index)
{
    return numbered(QStringLiteral("pages"), QStringLiteral("p"), index, QStringLiteral(".kra"));
}

QString PdfSession::thumbFileName(int index)
{
    return numbered(QStringLiteral("thumbs"), QStringLiteral("p"), index, QStringLiteral(".png"));
}

PdfSessionManifest PdfSession::createProject(const QString &projectDir,
                                             const QString &sourcePdf,
                                             PdfRenderBackend &backend,
                                             QString *why)
{
    if (!QFileInfo::exists(sourcePdf)) {
        fail(why, QStringLiteral("no such source: %1").arg(sourcePdf));
        return PdfSessionManifest();
    }

    const QDir dir(projectDir);
    if (dir.exists() && !dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty()) {
        fail(why, QStringLiteral("%1 already exists and is not empty").arg(projectDir));
        return PdfSessionManifest();
    }

    if (!backend.open(sourcePdf)) {
        fail(why, QStringLiteral("the renderer cannot open %1").arg(sourcePdf));
        return PdfSessionManifest();
    }

    if (!QDir().mkpath(dir.filePath(QStringLiteral("pages"))) ||
        !QDir().mkpath(dir.filePath(QStringLiteral("thumbs")))) {
        fail(why, QStringLiteral("cannot create the project directories under %1").arg(projectDir));
        return PdfSessionManifest();
    }

    const QString sourceFile = QFileInfo(sourcePdf).fileName();
    const QString copied = sourcePath(projectDir, sourceFile);
    if (!QFile::copy(sourcePdf, copied)) {
        fail(why, QStringLiteral("cannot copy the source into %1").arg(projectDir));
        return PdfSessionManifest();
    }

    PdfSessionManifest manifest;
    manifest.sourceFile = sourceFile;
    manifest.sourceSha256 = PdfSessionManifest::sha256OfFile(copied);
    manifest.sourceByteSize = QFileInfo(copied).size();

    for (int i = 0; i < backend.pageCount(); ++i) {
        const PdfPageInfo info = backend.pageInfo(i);
        if (!info.isValid()) {
            fail(why, QStringLiteral("page %1 has no usable geometry").arg(i + 1));
            return PdfSessionManifest();
        }

        PdfPageRecord page;
        page.index = info.index;
        page.sizePt = info.sizePt;
        page.rotation = info.rotation;
        page.kraFile = pageFileName(i);
        page.thumbFile = thumbFileName(i);
        page.generation = 0;
        manifest.pages.append(page);
    }

    if (!manifest.writeTo(manifestPath(projectDir), why)) {
        return PdfSessionManifest();
    }

    return manifest;
}

PdfSessionManifest PdfSession::openProject(const QString &projectDir, QString *why)
{
    const PdfSessionManifest manifest = PdfSessionManifest::readFrom(manifestPath(projectDir), why);
    if (!manifest.isValid(why)) {
        return PdfSessionManifest();
    }

    const QString source = sourcePath(projectDir, manifest.sourceFile);
    if (!QFileInfo::exists(source)) {
        fail(why, QStringLiteral("the source file %1 is missing").arg(manifest.sourceFile));
        return PdfSessionManifest();
    }

    /// The source is never written back to, so a checksum change means someone else edited
    /// or replaced it and the recorded page geometry can no longer be trusted.
    if (PdfSessionManifest::sha256OfFile(source) != manifest.sourceSha256) {
        fail(why, QStringLiteral("the source file %1 changed since the project was created")
                      .arg(manifest.sourceFile));
        return PdfSessionManifest();
    }

    return manifest;
}
