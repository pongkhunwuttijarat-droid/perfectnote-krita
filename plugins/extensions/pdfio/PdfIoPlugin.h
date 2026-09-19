/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFIOPLUGIN_H
#define PDFIOPLUGIN_H

#include <QString>

#include <KisActionPlugin.h>

/**
 * Entry point of the pdfio extension: PDF pages as locked backgrounds with an ink group on
 * top, plus a materialised note project around them.
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

    /**
     * Wraps a PDF into a note project and opens its first page in a view.
     *
     * Deliberately not private: the action and the probe both go through here, so whatever the
     * probe reports is the path the user takes.
     */
    bool openNotebook(const QString &pdfPath);

private Q_SLOTS:
    void slotOpenNotebook();
    void slotSavePage();
    void slotNextPage();
    void slotPreviousPage();
    void slotExportPdf();

private:
    void registerActions();

    /// Temporary measurement: opens N pages in a row and reports resident memory and the
    /// number of live documents after each, so the scaling with page count is visible rather
    /// than guessed.
    void runScaleProbe(int pages);
};

#endif // PDFIOPLUGIN_H
