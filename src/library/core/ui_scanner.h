#pragma once
#include <QObject>
#include <QJsonObject>
#include <QString>

// Shared serialization helpers for the snapshot / findElement RPC paths.
// Geometry and text are computed inside the target process (logical
// pixels) so both paths produce the same contract.
class UiScanner {
public:
    static QJsonObject rectToJson(QObject* obj);
    static QJsonObject globalRectToJson(QObject* obj);
    static QString displayText(QObject* obj);
    static bool isEffectivelyVisible(QObject* obj);
    static QObject* getVisualParent(QObject* obj);
    static int getZOrder(QObject* obj);
    static QObject* getContainingWindow(QObject* obj);
#ifdef QT_COMMANDER_WITH_QML
    /// QML id of the object (QQmlContext::nameForObject on the object's
    /// creation context); empty for non-QML objects or objects without an
    /// id.  The QML `id` is what QML developers reference in source -- it
    /// never shows up in QObject properties, only via this lookup.
    static QString qmlId(QObject* obj);
#endif
};
