/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfSessionManifest.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>

const int PdfSessionManifest::CurrentSchema = 1;

namespace {

QJsonArray sizeToJson(const QSizeF &size)
{
    return QJsonArray{size.width(), size.height()};
}

QSizeF sizeFromJson(const QJsonValue &value)
{
    const QJsonArray array = value.toArray();
    if (array.size() != 2) {
        return QSizeF();
    }
    return QSizeF(array.at(0).toDouble(), array.at(1).toDouble());
}

void fail(QString *why, const QString &message)
{
    if (why) {
        *why = message;
    }
}

} // namespace

bool PdfSessionManifest::isValid(QString *why) const
{
    if (schema != CurrentSchema) {
        fail(why, QStringLiteral("unsupported manifest schema %1").arg(schema));
        return false;
    }
    if (sourceFile.isEmpty()) {
        fail(why, QStringLiteral("no source file recorded"));
        return false;
    }
    if (sourceSha256.isEmpty()) {
        fail(why, QStringLiteral("no source checksum recorded"));
        return false;
    }
    if (pages.isEmpty()) {
        fail(why, QStringLiteral("no pages recorded"));
        return false;
    }
    for (const PdfPageRecord &page : pages) {
        if (page.index < 0 || !page.sizePt.isValid() || page.kraFile.isEmpty()) {
            fail(why, QStringLiteral("page %1 is incomplete").arg(page.index));
            return false;
        }
    }
    return true;
}

QJsonObject PdfSessionManifest::toJson() const
{
    QJsonArray pageArray;
    for (const PdfPageRecord &page : pages) {
        QJsonObject object;
        object.insert(QStringLiteral("index"), page.index);
        object.insert(QStringLiteral("sizePt"), sizeToJson(page.sizePt));
        object.insert(QStringLiteral("rotation"), page.rotation);
        object.insert(QStringLiteral("kra"), page.kraFile);
        object.insert(QStringLiteral("thumb"), page.thumbFile);
        object.insert(QStringLiteral("generation"), page.generation);
        pageArray.append(object);
    }

    QJsonObject source;
    source.insert(QStringLiteral("file"), sourceFile);
    source.insert(QStringLiteral("sha256"), QString::fromLatin1(sourceSha256));
    source.insert(QStringLiteral("bytes"), double(sourceByteSize));

    QJsonObject root;
    root.insert(QStringLiteral("schema"), schema);
    root.insert(QStringLiteral("source"), source);
    root.insert(QStringLiteral("pages"), pageArray);
    return root;
}

PdfSessionManifest PdfSessionManifest::fromJson(const QJsonObject &object, QString *why)
{
    PdfSessionManifest manifest;

    if (!object.contains(QStringLiteral("schema"))) {
        fail(why, QStringLiteral("not a pdfio manifest: no schema field"));
        return manifest;
    }

    manifest.schema = object.value(QStringLiteral("schema")).toInt();

    const QJsonObject source = object.value(QStringLiteral("source")).toObject();
    manifest.sourceFile = source.value(QStringLiteral("file")).toString();
    manifest.sourceSha256 = source.value(QStringLiteral("sha256")).toString().toLatin1();
    manifest.sourceByteSize = qint64(source.value(QStringLiteral("bytes")).toDouble());

    const QJsonArray pageArray = object.value(QStringLiteral("pages")).toArray();
    for (const QJsonValue &value : pageArray) {
        const QJsonObject pageObject = value.toObject();
        PdfPageRecord page;
        page.index = pageObject.value(QStringLiteral("index")).toInt(-1);
        page.sizePt = sizeFromJson(pageObject.value(QStringLiteral("sizePt")));
        page.rotation = pageObject.value(QStringLiteral("rotation")).toInt();
        page.kraFile = pageObject.value(QStringLiteral("kra")).toString();
        page.thumbFile = pageObject.value(QStringLiteral("thumb")).toString();
        page.generation = pageObject.value(QStringLiteral("generation")).toInt();
        manifest.pages.append(page);
    }

    if (!manifest.isValid(why)) {
        return manifest;
    }

    /// Clear a why that isValid() may have set on an earlier, discarded attempt.
    return manifest;
}

bool PdfSessionManifest::writeTo(const QString &path, QString *why) const
{
    if (!isValid(why)) {
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(why, QStringLiteral("cannot write %1").arg(path));
        return false;
    }

    file.write(QJsonDocument(toJson()).toJson(QJsonDocument::Indented));
    return true;
}

PdfSessionManifest PdfSessionManifest::readFrom(const QString &path, QString *why)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(why, QStringLiteral("cannot read %1").arg(path));
        return PdfSessionManifest();
    }

    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        fail(why, QStringLiteral("%1 is not valid JSON: %2").arg(path, error.errorString()));
        return PdfSessionManifest();
    }

    return fromJson(document.object(), why);
}

QByteArray PdfSessionManifest::sha256OfFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        return QByteArray();
    }
    return hash.result().toHex();
}
