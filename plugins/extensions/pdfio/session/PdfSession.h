/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFSESSION_H
#define PDFSESSION_H

#include "session/PdfSessionManifest.h"

#include <QString>

class PdfRenderBackend;

/**
 * The on-disk shape of a note project.
 *
 * A directory, not a single file, because a per page save then rewrites one small ink file
 * instead of the whole project, and a crash mid-save cannot take the other pages with it.
 * The single file form is reserved for backup and sharing, where rebuilding is safe.
 *
 *   <project>/manifest.json     schema, source identity, pages, generations
 *   <project>/source.pdf        the original bytes, copied once, never rewritten
 *   <project>/pages/pNNNN.kra   ink only, no merged image
 *   <project>/thumbs/pNNNN.png  small preview for the page list
 */
class PdfSession
{
public:
    /// Wraps an existing source PDF into a fresh project directory. Refuses to clobber.
    static PdfSessionManifest createProject(const QString &projectDir,
                                            const QString &sourcePdf,
                                            PdfRenderBackend &backend,
                                            QString *why = nullptr);

    /// Reads a project back and checks that its source is still the one recorded.
    static PdfSessionManifest openProject(const QString &projectDir, QString *why = nullptr);

    static QString manifestPath(const QString &projectDir);
    static QString sourcePath(const QString &projectDir, const QString &sourceFile);
    static QString pageFileName(int index);
    static QString thumbFileName(int index);
};

#endif // PDFSESSION_H
