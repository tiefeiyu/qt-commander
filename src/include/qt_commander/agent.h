#pragma once

#include <QString>

namespace qt_commander {

struct StartOptions {
    QString name;
    QString rendezvous_dir;
    QString token; // generated when empty
    QString mode = QStringLiteral("linked");
};

struct StartResult {
    bool ok = false;
    QString error;
    QString descriptor_path;
};

// Starts the in-process RPC server. Call on the GUI thread after
// QApplication has been constructed. Returns immediately.
StartResult start(const StartOptions& options);

// Stops the server and removes the descriptor. Safe to call more than once.
void stop();

} // namespace qt_commander

