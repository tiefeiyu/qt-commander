#pragma once
#include <QObject>
#include <QString>

// Screenshot capture for widgets and QML items.
class Screenshot {
public:
    // Take screenshot of element (or full window if target is a QWindow/QWidget window).
    // Returns path to saved PNG file, or empty string on error.
    static QString capture(QObject* target, const QString& screenshot_dir, int sequence);
};
