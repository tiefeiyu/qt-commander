// Modal/async regression target for the qt-commander task queue.
// Modes:
//   click-modal     : a button opens a modal QDialog via exec()
//   nested-modal    : A opens B, both via exec() (two nested event loops)
//   delete-on-close : the modal has WA_DeleteOnClose (dangling-pointer guard)
#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

#include "qt_commander/agent.h"

namespace {

QDialog* makeDialog(QWidget* parent, const QString& className,
                    const QString& title, const QString& buttonName,
                    const QString& buttonText)
{
    auto* dialog = new QDialog(parent);
    dialog->setObjectName(className);
    dialog->setWindowTitle(title);
    dialog->setModal(true);
    auto* layout = new QVBoxLayout(dialog);
    auto* close = new QPushButton(buttonText, dialog);
    close->setObjectName(buttonName);
    layout->addWidget(close);
    QObject::connect(close, &QPushButton::clicked, dialog, &QDialog::accept);
    dialog->resize(220, 100);
    return dialog;
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qt-modal-test"));

    const QStringList args = app.arguments();
    const int index = args.indexOf(QStringLiteral("--mode"));
    const QString mode = (index >= 0 && index + 1 < args.size())
                             ? args.at(index + 1)
                             : QStringLiteral("click-modal");

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("Qt Modal Test: %1").arg(mode));
    auto* central = new QWidget(&window);
    auto* layout = new QVBoxLayout(central);
    auto* status = new QLabel(QStringLiteral("idle"), central);
    status->setObjectName(QStringLiteral("statusLabel"));
    auto* fast = new QPushButton(QStringLiteral("Fast"), central);
    fast->setObjectName(QStringLiteral("btnFast"));
    QObject::connect(fast, &QPushButton::clicked, status,
                     [status]() { status->setText(QStringLiteral("clicked")); });
    layout->addWidget(status);
    layout->addWidget(fast);
    window.setCentralWidget(central);
    window.resize(420, 220);
    window.show();

    if (mode == QStringLiteral("click-modal")) {
        QDialog* dialog = makeDialog(&window, QStringLiteral("probeDialog"),
                                     QStringLiteral("Probe"),
                                     QStringLiteral("btnCloseDialog"),
                                     QStringLiteral("Close"));
        auto* open = new QPushButton(QStringLiteral("Open modal"), central);
        open->setObjectName(QStringLiteral("btnOpenModal"));
        layout->addWidget(open);
        QObject::connect(open, &QPushButton::clicked, dialog, &QDialog::exec);
    } else if (mode == QStringLiteral("nested-modal")) {
        QDialog* dialogA = makeDialog(&window, QStringLiteral("dialogA"),
                                      QStringLiteral("A"),
                                      QStringLiteral("btnCloseA"),
                                      QStringLiteral("Close A"));
        auto* openB = new QPushButton(QStringLiteral("Open B"), dialogA);
        openB->setObjectName(QStringLiteral("btnOpenB"));
        dialogA->layout()->addWidget(openB);
        QDialog* dialogB = makeDialog(dialogA, QStringLiteral("dialogB"),
                                      QStringLiteral("B"),
                                      QStringLiteral("btnCloseB"),
                                      QStringLiteral("Close B"));
        QObject::connect(openB, &QPushButton::clicked, dialogB, &QDialog::exec);
        auto* openA = new QPushButton(QStringLiteral("Open A"), central);
        openA->setObjectName(QStringLiteral("btnOpenA"));
        layout->addWidget(openA);
        QObject::connect(openA, &QPushButton::clicked, dialogA, &QDialog::exec);
    } else if (mode == QStringLiteral("delete-on-close")) {
        QDialog* dialog = makeDialog(&window, QStringLiteral("doomedDialog"),
                                     QStringLiteral("Doomed"),
                                     QStringLiteral("btnCloseDoomed"),
                                     QStringLiteral("Close"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        auto* open = new QPushButton(QStringLiteral("Open doomed"), central);
        open->setObjectName(QStringLiteral("btnOpenDoomed"));
        layout->addWidget(open);
        QObject::connect(open, &QPushButton::clicked, dialog, &QDialog::exec);
    }

    qt_commander::StartOptions options;
    options.name = QStringLiteral("qt-modal-test-%1").arg(mode);
    const QByteArray rendezvous = qgetenv("QT_COMMANDER_RENDEZVOUS");
    if (!rendezvous.isEmpty())
        options.rendezvous_dir = QString::fromLocal8Bit(rendezvous);
    qt_commander::start(options);
    return app.exec();
}
