#pragma once
#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <QSet>
#include <QString>
#include <cstdint>

class ElementMap;

// UI tree scanner. Traverses QWidget and QQuickItem trees, assigns IDs, generates snapshot JSON.
class UiScanner {
public:
    // Generate a full UI tree snapshot. Returns JSON with elements array.
    // detail: "core", "extended", or "full"
    // include_hidden: whether to include non-visible elements
    // element_map: will be populated with ID -> QObject* mappings (must not be null)
    // snapshot_dir: directory for saving binary property files (for "full" tier)
    // Returns a JSON object with keys: session_id, snapshot_id, epoch, timestamp_ms,
    //   element_count, detail, elements[], truncated (optional), truncated_reason (optional)
    static QJsonObject generateSnapshot(const QString& session_id,
                                         int snapshot_id,
                                         const QString& detail,
                                         bool include_hidden,
                                         ElementMap* element_map,
                                         const QString& snapshot_dir);

    // Shared serialization helpers -- used by findElement and the snapshot
    // RPC path so both produce the same geometry/visibility contract.
    static QJsonObject rectToJson(QObject* obj);
    static QJsonObject globalRectToJson(QObject* obj);
    static QString displayText(QObject* obj);
    static bool isEffectivelyVisible(QObject* obj);
    static QObject* getVisualParent(QObject* obj);
    static int getZOrder(QObject* obj);
    static QObject* getContainingWindow(QObject* obj);

private:
    static void traverseWidgets(QJsonArray& elements,
                                ElementMap* element_map,
                                QHash<QObject*, uint64_t>& id_map,
                                uint64_t& next_id,
                                QSet<QObject*>& visited,
                                int maxDepth,
                                const QString& detail,
                                bool include_hidden,
                                const QString& snapshot_dir,
                                bool& truncated,
                                QString& truncReason);

    static void traverseQmlWindows(QJsonArray& elements,
                                    ElementMap* element_map,
                                    QHash<QObject*, uint64_t>& id_map,
                                    uint64_t& next_id,
                                    QSet<QObject*>& visited,
                                    int maxDepth,
                                    const QString& detail,
                                    bool include_hidden,
                                    const QString& snapshot_dir,
                                    bool& truncated,
                                    QString& truncReason);

    static QJsonObject serializeElement(QObject* obj,
                                         QObject* parent,
                                         uint64_t id,
                                         const QString& detail,
                                         const QString& snapshot_dir,
                                         const QHash<QObject*, uint64_t>& id_map);

    static QJsonObject serializeProperties(QObject* obj,
                                            uint64_t element_id,
                                            const QString& detail,
                                            const QString& snapshot_dir);

    static QJsonValue propertyToJson(const QVariant& value,
                                      const QString& detail,
                                      const QString& snapshot_dir);
};
