#include <QApplication>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

int main(int argc, char** argv)
{
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

    window.setCentralWidget(central);
    window.resize(360, 180);
    window.show();
    return app.exec();
}
