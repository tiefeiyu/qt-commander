#include <QApplication>
#include <QComboBox>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSlider>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <QDebug>

#include "qt_commander/agent.h"

class CallProbe : public QPushButton {
    Q_OBJECT
public:
    using QPushButton::QPushButton;
    Q_INVOKABLE int add(int left, int right) { return left + right; }
    Q_INVOKABLE QString echo(const QString& text) { return text; }
    Q_INVOKABLE QWidget* self() { return this; }
};

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qt-linked-test"));

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("Qt Linked Test"));

    auto* central = new QWidget(&window);
    auto* layout = new QVBoxLayout(central);

    auto* top = new QPushButton(QStringLiteral("Alpha"), central);
    top->setObjectName(QStringLiteral("btnA"));
    layout->addWidget(top);

    auto* status = new QLabel(QStringLiteral("idle"), central);
    status->setObjectName(QStringLiteral("statusLabel"));
    layout->addWidget(status);

    auto* deleteButton = new QPushButton(QStringLiteral("Delete Alpha"), central);
    deleteButton->setObjectName(QStringLiteral("btnDelete"));
    layout->addWidget(deleteButton);
    QObject::connect(deleteButton, &QPushButton::clicked, top,
                     [top]() { top->deleteLater(); });

    auto* twinOne = new QPushButton(QStringLiteral("Twin 1"), central);
    twinOne->setObjectName(QStringLiteral("twinBtn"));
    layout->addWidget(twinOne);
    auto* twinTwo = new QPushButton(QStringLiteral("Twin 2"), central);
    twinTwo->setObjectName(QStringLiteral("twinBtn"));
    layout->addWidget(twinTwo);

    auto* tree = new QTreeWidget(central);
    tree->setObjectName(QStringLiteral("tree"));
    tree->setHeaderLabels({QStringLiteral("Name")});
    tree->addTopLevelItem(new QTreeWidgetItem({QStringLiteral("first")}));
    tree->addTopLevelItem(new QTreeWidgetItem({QStringLiteral("second")}));
    layout->addWidget(tree);

    auto* list = new QListWidget(central);
    list->setObjectName(QStringLiteral("list"));
    list->addItems({QStringLiteral("one"), QStringLiteral("two")});
    layout->addWidget(list);

    auto* scroll = new QScrollBar(Qt::Horizontal, central);
    scroll->setObjectName(QStringLiteral("scroll"));
    scroll->setRange(0, 100);
    scroll->setValue(40);
    layout->addWidget(scroll);

    auto* combo = new QComboBox(central);
    combo->setObjectName(QStringLiteral("combo"));
    combo->addItems({QStringLiteral("alpha"), QStringLiteral("beta"),
                     QStringLiteral("gamma")});
    layout->addWidget(combo);

    auto* tabs = new QTabWidget(central);
    tabs->setObjectName(QStringLiteral("mainTabs"));
    for (const QString& name : {QStringLiteral("One"), QStringLiteral("Two"),
                                QStringLiteral("Three")}) {
        auto* page = new QWidget(tabs);
        auto* pageLayout = new QVBoxLayout(page);
        auto* label = new QLabel(name, page);
        label->setObjectName(QStringLiteral("tabLabel_") + name);
        pageLayout->addWidget(label);
        tabs->addTab(page, name);
    }
    layout->addWidget(tabs);

    auto* table = new QTableWidget(2, 2, central);
    table->setObjectName(QStringLiteral("table"));
    table->setHorizontalHeaderLabels({QStringLiteral("A"), QStringLiteral("B")});
    table->setItem(0, 0, new QTableWidgetItem(QStringLiteral("a0")));
    table->setItem(0, 1, new QTableWidgetItem(QStringLiteral("b0")));
    table->setItem(1, 0, new QTableWidgetItem(QStringLiteral("a1")));
    table->setItem(1, 1, new QTableWidgetItem(QStringLiteral("b1")));
    table->horizontalHeader()->setObjectName(QStringLiteral("tableHeader"));
    layout->addWidget(table);

    auto* slider = new QSlider(Qt::Horizontal, central);
    slider->setObjectName(QStringLiteral("slider"));
    slider->setRange(0, 100);
    slider->setValue(40);
    layout->addWidget(slider);

    auto* box = new QGroupBox(QStringLiteral("Details"), central);
    box->setObjectName(QStringLiteral("detailsBox"));
    auto* boxLayout = new QVBoxLayout(box);
    auto* edit = new QLineEdit(box);
    edit->setObjectName(QStringLiteral("nameEdit"));
    edit->setText(QStringLiteral("initial"));
    boxLayout->addWidget(edit);
    layout->addWidget(box);

    auto* callProbe = new CallProbe(QStringLiteral("Call Probe"), central);
    callProbe->setObjectName(QStringLiteral("callProbe"));
    layout->addWidget(callProbe);

    QObject::connect(top, &QPushButton::clicked, status, [status]() {
        status->setText(QStringLiteral("clicked"));
    });

    window.setCentralWidget(central);

    window.menuBar()->setObjectName(QStringLiteral("menuBar"));
    auto* menu = window.menuBar()->addMenu(QStringLiteral("&File"));
    menu->setObjectName(QStringLiteral("fileMenu"));
    auto* quit = menu->addAction(QStringLiteral("Quit"));
    quit->setObjectName(QStringLiteral("actionQuit"));
    auto* about = menu->addAction(QStringLiteral("About"));
    about->setObjectName(QStringLiteral("actionAbout"));

    window.resize(480, 560);
    window.show();

    qt_commander::StartOptions options;
    options.name = QStringLiteral("linked");
    options.rendezvous_dir = qEnvironmentVariable("QT_COMMANDER_RENDEZVOUS");
    const auto result = qt_commander::start(options);
    if (!result.ok)
        qWarning() << "qt-commander start failed:" << result.error;
    else
        qInfo() << "qt-commander descriptor:" << result.descriptor_path;

    return app.exec();
}

#include "main.moc"
