#include "qt_commander/agent.h"

#include "server.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QSaveFile>
#include <QSysInfo>
#include <QUuid>
#include <QVariant>

#include <memory>

#ifndef QT_COMMANDER_BUILD_ID
#define QT_COMMANDER_BUILD_ID "unknown"
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#elif defined(Q_OS_UNIX)
#include <unistd.h>
#endif

namespace qt_commander {

namespace {

std::unique_ptr<Server> g_server;
QString g_descriptorPath;

QString makeToken()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString makeTag()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).left(4);
}

double processStartTime()
{
#ifdef Q_OS_WIN
    FILETIME creation;
    FILETIME exitTime;
    FILETIME kernelTime;
    FILETIME userTime;
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exitTime,
                         &kernelTime, &userTime))
        return 0.0;
    ULARGE_INTEGER value;
    value.LowPart = creation.dwLowDateTime;
    value.HighPart = creation.dwHighDateTime;
    constexpr quint64 epoch = 116444736000000000ULL;
    return double(value.QuadPart - epoch) / 10000000.0;
#elif defined(Q_OS_LINUX)
    QFile stat(QStringLiteral("/proc/self/stat"));
    if (!stat.open(QIODevice::ReadOnly))
        return 0.0;
    const QByteArray data = stat.readAll();
    const int close = data.lastIndexOf(')');
    if (close < 0)
        return 0.0;
    const QList<QByteArray> fields = data.mid(close + 2).split(' ');
    if (fields.size() <= 19)
        return 0.0;
    bool ok = false;
    const qint64 ticks = fields.at(19).toLongLong(&ok);
    if (!ok)
        return 0.0;
    qint64 bootTime = 0;
    QFile proc(QStringLiteral("/proc/stat"));
    if (proc.open(QIODevice::ReadOnly)) {
        for (const QByteArray& line : proc.readAll().split('\n')) {
            if (line.startsWith("btime ")) {
                bootTime = line.mid(6).trimmed().toLongLong();
                break;
            }
        }
    }
    long ticksPerSecond = sysconf(_SC_CLK_TCK);
    if (ticksPerSecond <= 0)
        ticksPerSecond = 100;
    return double(bootTime) + double(ticks) / double(ticksPerSecond);
#else
    return 0.0;
#endif
}

QJsonObject processScope()
{
    QJsonObject scope;
#ifdef Q_OS_UNIX
    scope.insert(QStringLiteral("uid"), int(getuid()));
    QFile pidNs(QStringLiteral("/proc/self/ns/pid"));
    if (pidNs.open(QIODevice::ReadOnly))
        scope.insert(QStringLiteral("pid_ns"), pidNs.readLink());
    QFile bootId(QStringLiteral("/proc/sys/kernel/random/boot_id"));
    if (bootId.open(QIODevice::ReadOnly)) {
        scope.insert(QStringLiteral("boot_id"),
                     QString::fromUtf8(bootId.readAll()).trimmed());
    }
#endif
    return scope;
}

} // namespace

StartResult start(const StartOptions& options)
{
    if (g_server)
        return {false, QStringLiteral("qt-commander is already running"),
                g_descriptorPath};
    if (!QCoreApplication::instance())
        return {false, QStringLiteral("QCoreApplication is required"), {}};

    const QString token = options.token.isEmpty() ? makeToken() : options.token;
    const QString tag = makeTag();
    auto server = std::make_unique<Server>(token, tag);

    quint16 port = 0;
    QString error;
    if (!server->listen(&port, &error))
        return {false, QStringLiteral("listen failed: %1").arg(error), {}};

    const QString dir = options.rendezvous_dir.isEmpty()
                            ? QDir::tempPath() + QStringLiteral("/qt-commander")
                            : options.rendezvous_dir;
    if (!QDir().mkpath(dir))
        return {false, QStringLiteral("cannot create %1").arg(dir), {}};
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                   QFileDevice::ExeOwner);

    const qint64 pid = QCoreApplication::applicationPid();
    const QString descriptorPath = QDir(dir).filePath(
        QStringLiteral("descriptor-%1.json").arg(pid));

    QJsonObject protocol;
    protocol.insert(QStringLiteral("major"), 2);
    protocol.insert(QStringLiteral("minor"), 0);

    QJsonObject qt;
    qt.insert(QStringLiteral("major"), QT_VERSION_MAJOR);
    qt.insert(QStringLiteral("version"), QString::fromLatin1(qVersion()));

    QJsonObject compiler;
#if defined(_MSC_VER)
    compiler.insert(QStringLiteral("family"), QStringLiteral("msvc"));
    compiler.insert(
        QStringLiteral("version"),
        QStringLiteral("%1.%2.%3")
            .arg(_MSC_FULL_VER / 10000000)
            .arg((_MSC_FULL_VER / 100000) % 100)
            .arg(_MSC_FULL_VER % 100000));
#elif defined(__clang__)
    compiler.insert(QStringLiteral("family"), QStringLiteral("clang"));
    compiler.insert(QStringLiteral("version"),
                    QStringLiteral("%1.%2.%3")
                        .arg(__clang_major__)
                        .arg(__clang_minor__)
                        .arg(__clang_patchlevel__));
#elif defined(__GNUC__)
#ifdef Q_OS_WIN
    compiler.insert(QStringLiteral("family"), QStringLiteral("mingw"));
#else
    compiler.insert(QStringLiteral("family"), QStringLiteral("gcc"));
#endif
    compiler.insert(QStringLiteral("version"),
                    QStringLiteral("%1.%2.%3")
                        .arg(__GNUC__)
                        .arg(__GNUC_MINOR__)
                        .arg(__GNUC_PATCHLEVEL__));
#else
    compiler.insert(QStringLiteral("family"), QStringLiteral("unknown"));
#endif

    QJsonObject descriptor;
    descriptor.insert(QStringLiteral("schema"), 2);
    descriptor.insert(QStringLiteral("protocol"), protocol);
    descriptor.insert(QStringLiteral("session_id"), tag);
    descriptor.insert(QStringLiteral("mode"), options.mode);
    descriptor.insert(QStringLiteral("pid"), pid);
    descriptor.insert(QStringLiteral("started_at"),
                      QDateTime::currentDateTimeUtc().toString(
                          Qt::ISODateWithMs));
    const double started = processStartTime();
    if (started > 0)
        descriptor.insert(QStringLiteral("process_start_time"), started);
    descriptor.insert(QStringLiteral("scope"), processScope());
    descriptor.insert(QStringLiteral("port"), int(port));
    descriptor.insert(QStringLiteral("token"), token);
    descriptor.insert(QStringLiteral("library_version"),
                      QStringLiteral("2.0.0"));
    descriptor.insert(QStringLiteral("library_build_id"),
                      QStringLiteral(QT_COMMANDER_BUILD_ID));
    descriptor.insert(QStringLiteral("qt"), qt);
    descriptor.insert(QStringLiteral("compiler"), compiler);
#ifdef Q_OS_WIN
    descriptor.insert(QStringLiteral("os"), QStringLiteral("windows"));
#else
    descriptor.insert(QStringLiteral("os"), QStringLiteral("linux"));
#endif
    const QString cpu = QSysInfo::buildCpuArchitecture();
    descriptor.insert(QStringLiteral("arch"),
                      cpu == QStringLiteral("x86_64") ? QStringLiteral("x64")
                      : cpu == QStringLiteral("i386") ? QStringLiteral("x86")
                      : cpu == QStringLiteral("arm64") ? QStringLiteral("arm64")
                                                       : cpu);
    descriptor.insert(QStringLiteral("capabilities"),
                      QJsonArray{QStringLiteral("query"), QStringLiteral("act"),
                                 QStringLiteral("screenshot"),
                                 QStringLiteral("get"), QStringLiteral("set"),
                                 QStringLiteral("call"), QStringLiteral("items"), QStringLiteral("source")});
    descriptor.insert(
        QStringLiteral("exe_path"),
        QFileInfo(QCoreApplication::applicationFilePath()).absoluteFilePath());
    descriptor.insert(QStringLiteral("nonce"), makeToken());

    QSaveFile file(descriptorPath);
    if (!file.open(QIODevice::WriteOnly)) {
        return {false, QStringLiteral("cannot write descriptor: %1")
                           .arg(file.errorString()),
                {}};
    }
    file.write(QJsonDocument(descriptor).toJson(QJsonDocument::Compact));
    if (!file.commit()) {
        return {false, QStringLiteral("cannot commit descriptor: %1")
                           .arg(file.errorString()),
                {}};
    }
    QFile::setPermissions(descriptorPath,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    static bool quitHookInstalled = false;
    if (!quitHookInstalled) {
        QObject::connect(QCoreApplication::instance(),
                         &QCoreApplication::aboutToQuit, []() { stop(); });
        quitHookInstalled = true;
    }

    g_server = std::move(server);
    g_descriptorPath = descriptorPath;
    return {true, {}, descriptorPath};
}

void stop()
{
    g_server.reset();
    if (!g_descriptorPath.isEmpty())
        QFile::remove(g_descriptorPath);
    g_descriptorPath.clear();
}

} // namespace qt_commander


