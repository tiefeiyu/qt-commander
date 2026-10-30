#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPainter>
#include <QPushButton>
#include <QQuickWidget>
#include <QSlider>
#include <QTabWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

#include "qt_commander/agent.h"

namespace {

class PaintedWidget : public QWidget {
public:
    explicit PaintedWidget(QWidget* parent = nullptr) : QWidget(parent)
    {
        setObjectName(QStringLiteral("customPainted"));
        setMinimumSize(220, 100);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(245, 245, 245));
        painter.setPen(QPen(QColor(30, 100, 200), 3));
        painter.drawEllipse(rect().adjusted(10, 10, -10, -10));
    }
};

QString argValue(const QStringList& args, const QString& name,
                 const QString& fallback)
{
    const int index = args.indexOf(name);
    return index >= 0 && index + 1 < args.size() ? args.at(index + 1) : fallback;
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qt-matrix-test"));
    const QString mode = argValue(app.arguments(), QStringLiteral("--mode"),
                                  QStringLiteral("basic"));

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("Qt Matrix: %1").arg(mode));
    auto* central = new QWidget(&window);
    auto* layout = new QVBoxLayout(central);

    if (mode == QStringLiteral("basic")) {
        auto* button = new QPushButton(QStringLiteral("Basic"), central);
        button->setObjectName(QStringLiteral("btnBasic"));
        layout->addWidget(button);
        auto* field = new QLineEdit(QStringLiteral("initial"), central);
        field->setObjectName(QStringLiteral("field"));
        layout->addWidget(field);
        auto* label = new QLabel(QStringLiteral("waiting"), central);
        label->setObjectName(QStringLiteral("label"));
        layout->addWidget(label);
        QObject::connect(button, &QPushButton::clicked, label,
                         [label]() { label->setText(QStringLiteral("clicked")); });
    } else if (mode == QStringLiteral("complex")) {
        auto* tabs = new QTabWidget(central);
        tabs->setObjectName(QStringLiteral("tabs"));
        auto* page = new QWidget(tabs);
        auto* pageLayout = new QVBoxLayout(page);
        auto* table = new QTableWidget(10, 4, page);
        table->setObjectName(QStringLiteral("table"));
        table->horizontalHeader()->setObjectName(QStringLiteral("tableHeader"));
        for (int row = 0; row < 10; ++row)
            for (int column = 0; column < 4; ++column)
                table->setItem(row, column,
                               new QTableWidgetItem(QStringLiteral("c%1%2")
                                                        .arg(row)
                                                        .arg(column)));
        pageLayout->addWidget(table);
        auto* tree = new QTreeWidget(page);
        tree->setObjectName(QStringLiteral("tree"));
        tree->setHeaderLabels({QStringLiteral("Name")});
        tree->addTopLevelItem(new QTreeWidgetItem({QStringLiteral("one")}));
        tree->addTopLevelItem(new QTreeWidgetItem({QStringLiteral("two")}));
        pageLayout->addWidget(tree);
        tabs->addTab(page, QStringLiteral("Data"));
        tabs->addTab(new QLabel(QStringLiteral("second page"), tabs),
                     QStringLiteral("Second"));
        layout->addWidget(tabs);
        auto* combo = new QComboBox(central);
        combo->setObjectName(QStringLiteral("combo"));
        combo->addItems({QStringLiteral("a"), QStringLiteral("b")});
        layout->addWidget(combo);
        auto* slider = new QSlider(Qt::Horizontal, central);
        slider->setObjectName(QStringLiteral("slider"));
        slider->setRange(0, 100);
        slider->setValue(50);
        layout->addWidget(slider);
    } else if (mode == QStringLiteral("custom-painted")) {
        layout->addWidget(new PaintedWidget(central));
        auto* button = new QPushButton(QStringLiteral("Custom"), central);
        button->setObjectName(QStringLiteral("btnCustom"));
        layout->addWidget(button);
    } else if (mode == QStringLiteral("multi-window")) {
        auto* button = new QPushButton(QStringLiteral("Main"), central);
        button->setObjectName(QStringLiteral("btnMain"));
        layout->addWidget(button);
        auto* dialog = new QDialog(&window);
        dialog->setObjectName(QStringLiteral("dialog"));
        dialog->setWindowTitle(QStringLiteral("Secondary"));
        auto* dialogLayout = new QVBoxLayout(dialog);
        auto* dialogButton = new QPushButton(QStringLiteral("Dialog"), dialog);
        dialogButton->setObjectName(QStringLiteral("btnDialog"));
        dialogLayout->addWidget(dialogButton);
        dialog->resize(260, 120);
        dialog->show();
    } else if (mode == QStringLiteral("occlusion")) {
        auto* top = new QPushButton(QStringLiteral("Top"), central);
        top->setObjectName(QStringLiteral("btnTop"));
        top->setGeometry(10, 10, 180, 60);
        auto* covered = new QPushButton(QStringLiteral("Covered"), central);
        covered->setObjectName(QStringLiteral("btnCovered"));
        covered->setGeometry(60, 40, 180, 60);
        layout->addWidget(new QLabel(QStringLiteral("overlap"), central));
    } else if (mode == QStringLiteral("qml") || mode == QStringLiteral("mixed")) {
        auto* quick = new QQuickWidget(central);
        quick->setObjectName(QStringLiteral("quick"));
        quick->setResizeMode(QQuickWidget::SizeRootObjectToView);
        quick->setSource(QUrl::fromLocalFile(
            QCoreApplication::applicationDirPath() + QStringLiteral("/main.qml")));
        layout->addWidget(quick);
        if (mode == QStringLiteral("mixed")) {
            auto* button = new QPushButton(QStringLiteral("Mixed"), central);
            button->setObjectName(QStringLiteral("btnMixed"));
            layout->addWidget(button);
        }
    }

    window.setCentralWidget(central);
    window.resize(640, 420);
    window.show();

    qt_commander::StartOptions options;
    options.name = QStringLiteral("matrix-%1").arg(mode);
    const QByteArray rendezvous = qgetenv("QT_COMMANDER_RENDEZVOUS");
    if (!rendezvous.isEmpty())
        options.rendezvous_dir = QString::fromLocal8Bit(rendezvous);
    qt_commander::start(options);
    return app.exec();
}
