/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfExporter.h"

#include <QFile>
#include <QRegularExpression>
#include <QTransform>

namespace {

/// A QByteArray, so that NL + "text" concatenates instead of doing pointer arithmetic.
const QByteArray NL("\n", 1);

void fail(QString *why, const QString &message)
{
    if (why) {
        *why = message;
    }
}

/// Object number -> body, for a file whose objects are all plain. Object streams are handled by
/// refusing them, not by guessing.
QHash<int, QByteArray> splitObjects(const QByteArray &pdf)
{
    QHash<int, QByteArray> objects;
    QRegularExpression re(QStringLiteral("(?:^|[\\r\\n])(\\d+)\\s+0\\s+obj\\s*(.*?)\\s*endobj"),
                          QRegularExpression::DotMatchesEverythingOption);
    QRegularExpressionMatchIterator it = re.globalMatch(QString::fromLatin1(pdf));
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        objects.insert(match.captured(1).toInt(), match.captured(2).toLatin1());
    }
    return objects;
}

QList<int> kidsOf(const QHash<int, QByteArray> &objects, int objectNumber)
{
    QList<int> kids;
    const QByteArray body = objects.value(objectNumber);
    const int at = body.indexOf("/Kids");
    if (at < 0) {
        return kids;
    }

    const int open = body.indexOf('[', at);
    const int close = body.indexOf(']', open);
    if (open < 0 || close < 0) {
        return kids;
    }

    const QString list = QString::fromLatin1(body.mid(open, close - open));
    QRegularExpression re(QStringLiteral("(\\d+)\\s+0\\s+R"));
    QRegularExpressionMatchIterator it = re.globalMatch(list);
    while (it.hasNext()) {
        kids.append(it.next().captured(1).toInt());
    }
    return kids;
}

/// FlateDecode wants raw zlib; qCompress prepends a four byte length that PDF does not expect.
QByteArray deflate(const QByteArray &data)
{
    return qCompress(data, 9).mid(4);
}

/// The ink as it has to sit in the page's own user space, which is not the space the user drew
/// in once /Rotate is not zero.
QImage inkInPageSpace(const QImage &displayInk, int rotation)
{
    const QImage source = displayInk.convertToFormat(QImage::Format_ARGB32);
    if (rotation == 0) {
        return source;
    }

    /// /Rotate turns the page clockwise for display, so the drawn image has to come back the
    /// other way to line up with the page's own coordinates.
    QTransform transform;
    transform.rotate(-rotation);
    return source.transformed(transform);
}

QByteArray rgbSamples(const QImage &image)
{
    const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    QByteArray samples;
    samples.reserve(rgb.width() * rgb.height() * 3);
    for (int y = 0; y < rgb.height(); ++y) {
        samples.append(reinterpret_cast<const char *>(rgb.constScanLine(y)), rgb.width() * 3);
    }
    return samples;
}

QByteArray alphaSamples(const QImage &image)
{
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    QByteArray samples;
    samples.reserve(argb.width() * argb.height());
    for (int y = 0; y < argb.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
        for (int x = 0; x < argb.width(); ++x) {
            samples.append(char(qAlpha(line[x])));
        }
    }
    return samples;
}

QByteArray imageObject(int width, int height, const QByteArray &colorSpace, const QByteArray &body,
                       bool withMask, int maskNumber)
{
    QByteArray object = "<< /Type /XObject /Subtype /Image /Width ";
    object += QByteArray::number(width) + " /Height " + QByteArray::number(height);
    object += " /ColorSpace /";
    object += colorSpace;
    object += " /BitsPerComponent 8 /Filter /FlateDecode";
    if (withMask) {
        object += " /SMask " + QByteArray::number(maskNumber) + " 0 R";
    }
    object += " /Length " + QByteArray::number(body.size()) + " >>" + NL + "stream" + NL;
    object += body;
    object += NL + "endstream";
    return object;
}

/// Reads \a key from a page dictionary as a rectangle of numbers.
bool mediaBox(const QByteArray &page, double *x0, double *y0, double *x1, double *y1)
{
    const int at = page.indexOf("/MediaBox");
    if (at < 0) {
        return false;
    }
    const int open = page.indexOf('[', at);
    const int close = page.indexOf(']', open);
    if (open < 0 || close < 0) {
        return false;
    }

    const QStringList parts = QString::fromLatin1(page.mid(open + 1, close - open - 1))
                                  .simplified().split(QLatin1Char(' '));
    if (parts.size() != 4) {
        return false;
    }
    *x0 = parts.at(0).toDouble();
    *y0 = parts.at(1).toDouble();
    *x1 = parts.at(2).toDouble();
    *y1 = parts.at(3).toDouble();
    return true;
}

int rotationOf(const QByteArray &page)
{
    const int at = page.indexOf("/Rotate");
    if (at < 0) {
        return 0;
    }
    const QString tail = QString::fromLatin1(page.mid(at + 7, 16)).simplified();
    return tail.section(QLatin1Char(' '), 0, 0).toInt();
}

} // namespace

QList<int> PdfExporter::pageObjectNumbers(const QByteArray &pdf, QString *why)
{
    if (pdf.contains("/Type /ObjStm") || pdf.contains("/Type/ObjStm")) {
        fail(why, QStringLiteral("this PDF keeps its objects in object streams, which the "
                                 "exporter cannot rewrite yet"));
        return {};
    }

    const QHash<int, QByteArray> objects = splitObjects(pdf);
    if (objects.isEmpty()) {
        fail(why, QStringLiteral("no plain objects found in the PDF"));
        return {};
    }

    /// From the trailer to the catalog to the page tree, rather than trusting file order.
    const int trailerAt = pdf.lastIndexOf("trailer");
    if (trailerAt < 0) {
        fail(why, QStringLiteral("the PDF has no trailer"));
        return {};
    }
    const QString trailer = QString::fromLatin1(pdf.mid(trailerAt, 400));
    const QRegularExpression rootRe(QStringLiteral("/Root\\s+(\\d+)\\s+0\\s+R"));
    const QRegularExpressionMatch rootMatch = rootRe.match(trailer);
    if (!rootMatch.hasMatch()) {
        fail(why, QStringLiteral("the trailer has no /Root"));
        return {};
    }

    const QByteArray catalog = objects.value(rootMatch.captured(1).toInt());
    const QRegularExpression pagesRe(QStringLiteral("/Pages\\s+(\\d+)\\s+0\\s+R"));
    const QRegularExpressionMatch pagesMatch = pagesRe.match(QString::fromLatin1(catalog));
    if (!pagesMatch.hasMatch()) {
        fail(why, QStringLiteral("the catalog has no /Pages"));
        return {};
    }

    QList<int> pages;
    QList<int> pending{ pagesMatch.captured(1).toInt() };
    while (!pending.isEmpty()) {
        const int number = pending.takeFirst();
        const QByteArray body = objects.value(number);
        /// Not /Pages: a naive test for /Page also matches the page tree node, which would be
        /// listed as a page and its children never visited.
        static const QRegularExpression pageRe(QStringLiteral("/Type\\s*/Page(?![s])"));
        if (pageRe.match(QString::fromLatin1(body)).hasMatch()) {
            pages.append(number);
            continue;
        }
        pending = kidsOf(objects, number) + pending;
    }

    if (pages.isEmpty()) {
        fail(why, QStringLiteral("no pages found in the page tree"));
    }
    return pages;
}

bool PdfExporter::exportWithInk(const QString &sourcePdf,
                                const PdfSessionManifest &manifest,
                                const QHash<int, QImage> &ink,
                                const QString &outPath,
                                QString *why)
{
    if (!manifest.isValid(why)) {
        return false;
    }

    QFile source(sourcePdf);
    if (!source.open(QIODevice::ReadOnly)) {
        fail(why, QStringLiteral("cannot read %1").arg(sourcePdf));
        return false;
    }
    const QByteArray pdf = source.readAll();
    source.close();

    const QList<int> pages = pageObjectNumbers(pdf, why);
    if (pages.isEmpty()) {
        return false;
    }

    const QHash<int, QByteArray> objects = splitObjects(pdf);

    /// /Size from the trailer, so appended objects start above everything that exists.
    const QString trailer = QString::fromLatin1(pdf.mid(pdf.lastIndexOf("trailer"), 400));
    const QRegularExpression sizeRe(QStringLiteral("/Size\\s+(\\d+)"));
    const QRegularExpressionMatch sizeMatch = sizeRe.match(trailer);
    int nextObject = sizeMatch.hasMatch() ? sizeMatch.captured(1).toInt() : objects.size() + 1;

    const QRegularExpression rootRe(QStringLiteral("/Root\\s+(\\d+)\\s+0\\s+R"));
    const QRegularExpressionMatch rootMatch = rootRe.match(trailer);
    if (!rootMatch.hasMatch()) {
        fail(why, QStringLiteral("the trailer has no /Root"));
        return false;
    }
    const int rootObject = rootMatch.captured(1).toInt();

    /// Where the previous xref is, so the new trailer can chain to it.
    int prevXrefOffset = 0;
    {
        const int startxrefAt = pdf.lastIndexOf("startxref");
        if (startxrefAt >= 0) {
            const QString tail = QString::fromLatin1(pdf.mid(startxrefAt + 9, 32)).simplified();
            prevXrefOffset = tail.section(QLatin1Char(' '), 0, 0).toInt();
        }
    }

    QByteArray out = pdf;
    if (!out.endsWith(NL)) {
        out += NL;
    }

    QHash<int, QByteArray> replacements;   // object number -> new body
    QList<QPair<int, QByteArray>> appended; // new object number -> body, in writable order

    for (int i = 0; i < pages.size() && i < manifest.pages.size(); ++i) {
        const int pageObject = pages.at(i);
        const QByteArray page = objects.value(pageObject);
        if (page.isEmpty()) {
            fail(why, QStringLiteral("page %1 is missing from the file").arg(i + 1));
            return false;
        }

        const PdfPageRecord &record = manifest.pages.at(i);
        QHash<int, QImage>::const_iterator found = ink.constFind(i);
        if (found == ink.constEnd() || found->isNull()) {
            continue;
        }

        double x0 = 0;
        double y0 = 0;
        double x1 = 0;
        double y1 = 0;
        if (!mediaBox(page, &x0, &y0, &x1, &y1)) {
            fail(why, QStringLiteral("page %1 has no usable MediaBox").arg(i + 1));
            return false;
        }

        /// The image is stored the way the viewer will rotate it, so it has to be turned back
        /// into the page's own coordinates first.
        QImage placed = inkInPageSpace(*found, rotationOf(page));
        if (placed.isNull()) {
            fail(why, QStringLiteral("page %1 produced no image").arg(i + 1));
            return false;
        }

        const int maskNumber = nextObject++;
        const int imageNumber = nextObject++;
        appended.append({ maskNumber,
                          imageObject(placed.width(), placed.height(), "DeviceGray",
                                      deflate(alphaSamples(placed)), false, 0) });
        appended.append({ imageNumber,
                          imageObject(placed.width(), placed.height(), "DeviceRGB",
                                      deflate(rgbSamples(placed)), true, maskNumber) });

        /// Draw the ink over the page, in the page's own user space.
        const double widthPt = x1 - x0;
        const double heightPt = y1 - y0;
        QByteArray content = "q" + QByteArray::number(widthPt, 'f', 4) + " 0 0 "
                             + QByteArray::number(heightPt, 'f', 4) + " "
                             + QByteArray::number(x0, 'f', 4) + " "
                             + QByteArray::number(y0, 'f', 4) + " cm /pdfioInk Do Q" + NL;

        const int contentNumber = nextObject++;
        appended.append({ contentNumber,
                          "<< /Length " + QByteArray::number(content.size()) + " >>" + NL
                              + "stream" + NL + content + "endstream" });

        /// /Contents becomes the original plus ours, so the ink lands on top.
        QByteArray replaced = page;
        const int contentsAt = replaced.indexOf("/Contents");
        if (contentsAt < 0) {
            fail(why, QStringLiteral("page %1 has no /Contents").arg(i + 1));
            return false;
        }

        const int valueStart = contentsAt + 9;
        int valueEnd = valueStart;
        if (replaced.mid(valueStart, 1) == "[") {
            valueEnd = replaced.indexOf(']', valueStart) + 1;
        } else {
            while (valueEnd < replaced.size()
                   && (replaced.at(valueEnd) == ' '
                       || (replaced.at(valueEnd) >= '0' && replaced.at(valueEnd) <= '9'))) {
                ++valueEnd;
            }
            /// Consume the object reference tail: "N 0 R".
            const int rAt = replaced.indexOf(" R", valueEnd);
            valueEnd = rAt >= 0 ? rAt + 2 : valueEnd;
        }
        const QByteArray originalContents = replaced.mid(valueStart, valueEnd - valueStart);
        QByteArray list = originalContents.startsWith("[")
                              ? originalContents.left(originalContents.size() - 1)
                              : QByteArray("[") + originalContents;
        list += " " + QByteArray::number(contentNumber) + " 0 R ]";
        replaced.replace(valueStart, valueEnd - valueStart, list);

        /// The drawing operator needs a name in the page's resources.
        const int resourcesAt = replaced.indexOf("/Resources");
        if (resourcesAt < 0) {
            fail(why, QStringLiteral("page %1 has no /Resources").arg(i + 1));
            return false;
        }
        /// Skip the whitespace between the key and its value: "/Resources << ..." is the
        /// normal spelling and a check at a fixed offset misses it.
        int resourcesValue = resourcesAt + 10;
        while (resourcesValue < replaced.size() && replaced.at(resourcesValue) == ' ') {
            ++resourcesValue;
        }

        const QByteArray xobjectEntry = "/XObject << /pdfioInk " + QByteArray::number(imageNumber) + " 0 R >> ";
        if (replaced.mid(resourcesValue, 2) == "<<") {
            replaced.insert(resourcesValue + 2, xobjectEntry);
        } else {
            fail(why, QStringLiteral("page %1 refers to its resources indirectly, which the "
                                     "exporter does not merge yet").arg(i + 1));
            return false;
        }

        Q_UNUSED(record);
        replacements.insert(pageObject, replaced);
    }

    if (appended.isEmpty() && replacements.isEmpty()) {
        /// Nothing to add: the honest answer is a clean copy, not an incremental update with an
        /// empty body.
        QFile copy(outPath);
        if (!copy.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            fail(why, QStringLiteral("cannot write %1").arg(outPath));
            return false;
        }
        copy.write(pdf);
        return true;
    }

    QList<int> written;
    QHash<int, int> offsets;
    for (auto it = replacements.constBegin(); it != replacements.constEnd(); ++it) {
        offsets.insert(it.key(), out.size());
        out += QByteArray::number(it.key()) + " 0 obj" + NL + it.value() + NL + "endobj" + NL;
        written.append(it.key());
    }
    for (const QPair<int, QByteArray> &entry : appended) {
        offsets.insert(entry.first, out.size());
        out += QByteArray::number(entry.first) + " 0 obj" + NL + entry.second + NL + "endobj" + NL;
        written.append(entry.first);
    }

    std::sort(written.begin(), written.end());

    const int xrefAt = out.size();
    out += "xref" + QByteArray(NL);
    for (int number : written) {
        out += QByteArray::number(number) + " 1" + NL;
        out += QByteArray::number(offsets.value(number)).rightJustified(10, '0') + " 00000 n " + NL;
    }
    out += "trailer" + QByteArray(NL) + "<< /Size " + QByteArray::number(nextObject)
           + " /Root " + QByteArray::number(rootObject) + " 0 R /Prev "
           + QByteArray::number(prevXrefOffset) + " >>" + NL
           + "startxref" + QByteArray(NL) + QByteArray::number(xrefAt) + NL + "%%EOF" + NL;

    QFile target(outPath);
    if (!target.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(why, QStringLiteral("cannot write %1").arg(outPath));
        return false;
    }
    target.write(out);
    return true;
}
