/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFIOPROBE_H
#define PDFIOPROBE_H

/**
 * Temporary end to end probe, run from inside the application.
 *
 * A bare QTest cannot drive KisPart or KraConverter: both want the resource servers and
 * document storage that only a running Krita has, and asking anyway hung instead of failing.
 * So the whole open and save path is exercised here, in the process where it will actually
 * run, and reports through qDebug.
 *
 * Enabled with PDFIO_PROBE=<file.pdf>; does nothing otherwise.
 */
namespace PdfIoProbe {
void runIfRequested();
}

#endif // PDFIOPROBE_H
