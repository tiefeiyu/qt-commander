#pragma once
#include <QJsonObject>
#include <cstdint>

/// Parse element ID from JSON params.
///
/// Accepts both camelCase (``elementId``) and snake_case (``element_id``),
/// as well as direct integer values.
///
/// Returns true and sets *outId* on success; returns false otherwise.
inline bool qt_parse_element_id(const QJsonObject& params, uint64_t& outId) {
    QJsonValue val = params.value(QStringLiteral("elementId"));
    if (val.isUndefined())
        val = params.value(QStringLiteral("element_id"));
    if (val.isUndefined())
        return false;
    if (val.isDouble()) {
        outId = static_cast<uint64_t>(val.toDouble());
        return outId > 0;
    }
    const QString idStr = val.toString();
    if (idStr.isEmpty())
        return false;
    bool ok = false;
    outId = idStr.toULongLong(&ok);
    return ok;
}
