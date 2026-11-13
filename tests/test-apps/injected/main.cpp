#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

#include "../common/mouse_probe.h"

#ifdef Q_OS_LINUX
#include <sys/prctl.h>
#endif

int main(int argc, char** argv)
{
#ifdef Q_OS_LINUX
    // Allow a sibling injector under Yama ptrace_scope=1 for this test app.
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
#endif
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qt-injected-test"));

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("Qt Injected Test"));
    auto* central = new QWidget(&window);
    auto* layout = new QVBoxLayout(central);

    auto* button = new QPushButton(QStringLiteral("Injected"), central);
    button->setObjectName(QStringLiteral("injectedBtn"));
    layout->addWidget(button);

    auto* status = new QLabel(QStringLiteral("idle"), central);
    status->setObjectName(QStringLiteral("statusLabel"));
    layout->addWidget(status);
    QObject::connect(button, &QPushButton::clicked, status,
                     [status]() { status->setText(QStringLiteral("clicked")); });

    // Records what the runtime dispatched: button, held mask, modifiers,
    // hover transitions and context menus.
    auto* probe = new MouseProbe(central);
    probe->setObjectName(QStringLiteral("mouseProbe"));
    probe->setText(QStringLiteral("none"));
    layout->addWidget(probe);

    // A dialog whose Close button destroys the dialog while the click is still
    // being dispatched. This is the shape that used to crash the injected
    // runtime: the action killed its own target, and the post-action
    // observation then dereferenced the freed widget.
    auto* openButton = new QPushButton(QStringLiteral("Open dialog"), central);
    openButton->setObjectName(QStringLiteral("openDialogBtn"));
    layout->addWidget(openButton);
    QObject::connect(openButton, &QPushButton::clicked, &window,
                     [&window, status]() {
                         auto* dialog = new QDialog(&window);
                         dialog->setObjectName(
                             QStringLiteral("selfDestructDialog"));
                         auto* dialogLayout = new QVBoxLayout(dialog);
                         auto* closeButton =
                             new QPushButton(QStringLiteral("Close"), dialog);
                         closeButton->setObjectName(QStringLiteral("closeBtn"));
                         dialogLayout->addWidget(closeButton);
                         QObject::connect(
                             closeButton, &QPushButton::clicked, dialog,
                             [dialog, status]() {
                                 status->setText(
                                     QStringLiteral("dialog closed"));
                                 dialog->deleteLater();
                             });
                         dialog->show();
                     });

    window.setCentralWidget(central);
    window.resize(360, 220);
    window.show();
    return app.exec();
}
