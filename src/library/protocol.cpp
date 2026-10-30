#include "protocol.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QtEndian>

namespace qt_commander {

namespace {

void writeU32(QByteArray* out, quint32 value)
{
    char bytes[4];
    qToBigEndian<quint32>(value, reinterpret_cast<uchar*>(bytes));
    out->append(bytes, 4);
}

quint32 readU32(const QByteArray& data, int offset)
{
    return qFromBigEndian<quint32>(
        reinterpret_cast<const uchar*>(data.constData() + offset));
}

} // namespace

QByteArray encodeFrame(const QJsonObject& message, const QByteArray& binary)
{
    const QByteArray json = QJsonDocument(message).toJson(QJsonDocument::Compact);
    QByteArray out;
    out.reserve(kFrameHeaderSize + json.size() + binary.size());
    writeU32(&out, kFrameMagic);
    writeU32(&out, static_cast<quint32>(json.size()));
    writeU32(&out, static_cast<quint32>(binary.size()));
    out.append(json);
    out.append(binary);
    return out;
}

void FrameParser::append(const QByteArray& bytes)
{
    buffer_.append(bytes);
}

bool FrameParser::next(Frame* frame, QString* error)
{
    if (buffer_.size() < kFrameHeaderSize)
        return false;

    const quint32 magic = readU32(buffer_, 0);
    const quint32 jsonLength = readU32(buffer_, 4);
    const quint32 binaryLength = readU32(buffer_, 8);
    if (magic != kFrameMagic) {
        if (error)
            *error = QStringLiteral("bad frame magic");
        return false;
    }
    if (jsonLength == 0) {
        if (error)
            *error = QStringLiteral("empty JSON payload");
        return false;
    }
    const quint64 total = kFrameHeaderSize + quint64(jsonLength) + binaryLength;
    if (total > quint64(kMaxFrameBytes)) {
        if (error)
            *error = QStringLiteral("frame too large");
        return false;
    }
    if (quint64(buffer_.size()) < total)
        return false;

    const QByteArray json = buffer_.mid(kFrameHeaderSize, int(jsonLength));
    const QByteArray binary = buffer_.mid(kFrameHeaderSize + int(jsonLength),
                                          int(binaryLength));
    buffer_.remove(0, int(total));

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = QStringLiteral("invalid JSON payload: %1")
                         .arg(parseError.errorString());
        return false;
    }
    frame->message = document.object();
    frame->binary = binary;
    return true;
}

} // namespace qt_commander

