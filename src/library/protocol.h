#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace qt_commander {

constexpr quint32 kFrameMagic = 0x51544332; // "QTC2"
constexpr int kFrameHeaderSize = 12;
constexpr int kMaxFrameBytes = 64 * 1024 * 1024;

struct Frame {
    QJsonObject message;
    QByteArray binary;
};

QByteArray encodeFrame(const QJsonObject& message,
                       const QByteArray& binary = QByteArray());

// Incremental parser for one connection. append() buffers bytes; next()
// returns true for each complete frame and false when more data is needed.
// On a malformed frame it returns false and sets *error.
class FrameParser {
public:
    void append(const QByteArray& bytes);
    bool next(Frame* frame, QString* error);

private:
    QByteArray buffer_;
};

} // namespace qt_commander

