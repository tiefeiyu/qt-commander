#pragma once
#include <QObject>
#include <QVariant>
#include <QVector>
#include <QJsonObject>
#include <QJsonArray>

struct SelectorResult {
    uint64_t id;
    QObject* object;
};

class ElementSelector {
public:
    static QVector<SelectorResult> find(const QJsonObject& query,
                                         const QHash<uint64_t, QObject*>& element_map);
    static bool matchesQuery(QObject* obj, const QJsonObject& query,
                             const QHash<uint64_t, QObject*>& element_map,
                             uint64_t query_ancestor_id, uint64_t query_window_id);

private:
    static QVariant getPropertyValue(QObject* obj, const QString& name);
    static QString getDisplayText(QObject* obj);
    static QObject* getContainingWindow(QObject* obj);
    static int getDepthFromAncestor(QObject* obj, QObject* ancestor);
};
