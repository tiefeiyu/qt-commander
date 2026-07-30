#pragma once
#include <cstdint>
#include <string>
#include <atomic>
#include <thread>
#include <QObject>
#include <QSemaphore>
#include <QVariant>
#include <QJsonObject>
#include <QJsonDocument>
#include "common/socket_utils.h"
#include <QReadWriteLock>

#include "api.h"
class Handler;

// TCP JSON-RPC server running inside the injected library.
// Runs on a worker thread. Dispatches widget operations to main thread via Qt::QueuedConnection.
class RpcServer : public QObject {
    Q_OBJECT
public:
    explicit RpcServer(QObject* parent = nullptr);
    ~RpcServer();

    // Initialize with params from injector. Starts the TCP listener and worker thread.
    // Returns 0 on success, -1 on error.
    int start(const InitParams* params);

    // Graceful shutdown: close TCP, signal worker, join thread.
    void shutdown();

    bool isRunning() const { return running_.load(); }

    // Access the element map (shared with UI scanner, event injector)
    class ElementMap* elementMap() { return element_map_.get(); }
    const ElementMap* elementMap() const { return element_map_.get(); }

    // Accessors needed by Handler
    QString workspacePath() const { return QString::fromStdString(workspace_path_); }
    QString sessionId() const { return QString::fromStdString(session_id_); }

signals:
    void operationCompleted();

private:
    void workerLoop();
    std::string processRequest(const QJsonObject& request);
    bool authenticateConnection(socket_t client);
    std::string readFrame(socket_t client);
    bool sendFrame(socket_t client, const std::string& data);

    // Dispatch an operation to the main thread and wait for result (30s timeout)
    QVariant dispatchToMain(const std::string& method, const QJsonObject& params);

    // Helper: resolve elementId string from params to uint64_t, capture epoch
    struct DispatchArgs {
        uint64_t elementId = 0;
        uint64_t capturedEpoch = 0;
        QJsonObject operationParams;
    };
    DispatchArgs prepareDispatch(const QJsonObject& params);

    // RPC method: element-related operations sent TO the target process BY the MCP server
    // (These are the actual operations the AI agent wants to perform)
    QJsonObject handleSnapshot(const QJsonObject& params);
    QJsonObject handleFindElement(const QJsonObject& params);
    QJsonObject handleGetProperty(const QJsonObject& params);
    QJsonObject handleSetProperty(const QJsonObject& params);
    QJsonObject handleCallMethod(const QJsonObject& params);
    QJsonObject handleScreenshot(const QJsonObject& params);
    QJsonObject handleMouseClick(const QJsonObject& params);
    QJsonObject handleMousePress(const QJsonObject& params);
    QJsonObject handleMouseRelease(const QJsonObject& params);
    QJsonObject handleMouseDblClick(const QJsonObject& params);
    QJsonObject handleMouseMove(const QJsonObject& params);
    QJsonObject handleMouseWheel(const QJsonObject& params);
    QJsonObject handleKeyPress(const QJsonObject& params);
    QJsonObject handleKeyRelease(const QJsonObject& params);
    QJsonObject handleTypeText(const QJsonObject& params);
    QJsonObject handleKeyCombo(const QJsonObject& params);
    QJsonObject handleFocus(const QJsonObject& params);
    QJsonObject handleClearFocus(const QJsonObject& params);
    QJsonObject handleContextMenu(const QJsonObject& params);
    QJsonObject handleTouchPress(const QJsonObject& params);
    QJsonObject handleTouchMove(const QJsonObject& params);
    QJsonObject handleTouchRelease(const QJsonObject& params);

    // Auth handling
    QJsonObject handleAuthenticate(const QJsonObject& params);

    // Shutdown notification (no response)
    void handleShutdown();

    socket_t listen_fd_ = INVALID_SOCK;
    socket_t client_fd_ = INVALID_SOCK;
    std::atomic<bool> running_{false};
    std::atomic<bool> operation_in_progress_{false};
    std::thread worker_thread_;
    std::string token_;
    std::string workspace_path_;
    std::string session_id_;
    std::string port_file_path_;

    std::unique_ptr<ElementMap> element_map_;
    Handler* handler_ = nullptr;

    int request_counter_ = 0;
    mutable QReadWriteLock rw_lock_;  // For element map access
};
