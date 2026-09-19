/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFRENDERERSPIKE_H
#define PDFRENDERERSPIKE_H

/**
 * Temporary probe, to be removed once the answer is recorded.
 *
 * Question: can the Android render backend live entirely in this plugin, with no Java of
 * our own in packaging/android/apk/src/? android.graphics.pdf.PdfRenderer is a framework
 * class, not an OEM service, so QJniObject should reach it directly.
 *
 * The probe writes a one page PDF whose only content is a black square in the bottom-left
 * corner of the media box, renders it through PdfRenderer and samples the four corners of
 * the resulting bitmap, so the log says both "it rendered" and "in the right orientation".
 */
namespace PdfRendererSpike {
void run();
}

#endif // PDFRENDERERSPIKE_H
