/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ANDROIDDOCUMENTPICKER_H
#define ANDROIDDOCUMENTPICKER_H

#include <QObject>
#include <QString>

#include <functional>

/**
 * Asks Android for a PDF and hands back a path the renderer can open.
 *
 * QFileDialog is not usable for this: the application has no broad filesystem access on a modern
 * device, so the file arrives as a content:// URI instead of a path. Nothing of the vendor or of
 * ours is added to the APK for it -- Qt's own activity-result plumbing is enough -- but the
 * content still has to be copied out, because PdfRenderer needs a seekable descriptor and a
 * content URI only offers a stream.
 */
class AndroidDocumentPicker : public QObject
{
    Q_OBJECT
public:
    explicit AndroidDocumentPicker(QObject *parent = nullptr);
    ~AndroidDocumentPicker() override;

    /**
     * Opens the system picker. \a onPicked is called with the local copy, or with an empty string
     * and a reason when the user cancels or the copy fails.
     */
    void pickPdf(std::function<void(const QString &localPath, const QString &why)> onPicked);

private:
    struct Private;
    Private *d;
};

#endif // ANDROIDDOCUMENTPICKER_H
