// Deterministic large QWidget test application for P0 benchmarks.
//
// Node count is controlled by --nodes; the table model contributes rows*cols
// non-QObject cells, and --hidden adds children under a non-current stack page
// (present in the object tree, pruned from snapshots unless include_hidden).
#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QFile>
#include <QGridLayout>
#include <QHeaderView>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableView>
#include <QTextStream>
#include <QTimer>
#include <QWidget>

#include <QDebug>

#include "qt_commander/agent.h"

namespace {

QWidget* makeGrid(int count, QWidget* parent)
{
    auto* area = new QScrollArea(parent);
    area->setObjectName(QStringLiteral("gridArea"));
    area->setWidgetResizable(true);

    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("gridContent"));
    auto* grid = new QGridLayout(content);
    grid->setSpacing(2);

    constexpr int kColumns = 20;
    for (int i = 0; i < count; ++i) {
        auto* button = new QPushButton(QStringLiteral("Button %1").arg(i), content);
        button->setObjectName(QStringLiteral("btn_%1").arg(i));
        grid->addWidget(button, i / kColumns, i % kColumns);
    }

    area->setWidget(content);
    return area;
}

QWidget* makeTable(int rows, int columns, QWidget* parent)
{
    auto* view = new QTableView(parent);
    view->setObjectName(QStringLiteral("largeTable"));

    auto* model = new QStandardItemModel(rows, columns, view);
    for (int c = 0; c < columns; ++c)
        model->setHorizontalHeaderItem(
            c, new QStandardItem(QStringLiteral("Col %1").arg(c)));
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < columns; ++c) {
            model->setItem(
                r, c,
                new QStandardItem(QStringLiteral("%1,%2").arg(r).arg(c)));
        }
    }
    view->setModel(model);
    view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    return view;
}

QWidget* makeStack(int hiddenCount, QWidget* parent)
{
    auto* stack = new QStackedWidget(parent);
    stack->setObjectName(QStringLiteral("stack"));

    auto* visible = new QWidget;
    visible->setObjectName(QStringLiteral("stackVisible"));
    auto* visibleLayout = new QGridLayout(visible);
    auto* visibleButton = new QPushButton(QStringLiteral("Visible page"), visible);
    visibleButton->setObjectName(QStringLiteral("visiblePageButton"));
    visibleLayout->addWidget(visibleButton);
    stack->addWidget(visible);

    auto* hidden = new QWidget;
    hidden->setObjectName(QStringLiteral("stackHidden"));
    auto* hiddenLayout = new QGridLayout(hidden);
    for (int i = 0; i < hiddenCount; ++i) {
        auto* button = new QPushButton(QStringLiteral("Hidden %1").arg(i), hidden);
        button->setObjectName(QStringLiteral("hiddenBtn_%1").arg(i));
        hiddenLayout->addWidget(button, i / 20, i % 20);
    }
    stack->addWidget(hidden);
    stack->setCurrentIndex(0);
    return stack;
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qt-large-test"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Deterministic large QWidget app for qt-commander P0"));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("nodes"), QStringLiteral("Grid buttons."),
                      QStringLiteral("count"), QStringLiteral("1000")});
    parser.addOption({QStringLiteral("hidden"),
                      QStringLiteral("Hidden stack-page buttons."),
                      QStringLiteral("count"), QStringLiteral("500")});
    parser.addOption({QStringLiteral("rows"), QStringLiteral("Table rows."),
                      QStringLiteral("count"), QStringLiteral("500")});
    parser.addOption({QStringLiteral("cols"), QStringLiteral("Table columns."),
                      QStringLiteral("count"), QStringLiteral("20")});
    parser.addOption({QStringLiteral("ready-file"),
                      QStringLiteral("Write this file once the window is shown."),
                      QStringLiteral("path")});
    parser.process(app);

    const int nodes = parser.value(QStringLiteral("nodes")).toInt();
    const int hidden = parser.value(QStringLiteral("hidden")).toInt();
    const int rows = parser.value(QStringLiteral("rows")).toInt();
    const int cols = parser.value(QStringLiteral("cols")).toInt();
    const QString readyFile = parser.value(QStringLiteral("ready-file"));

    QMainWindow window;
    window.setObjectName(QStringLiteral("mainWindow"));
    window.setWindowTitle(
        QStringLiteral("Qt Large Test App (%1 grid, %2 hidden, %3x%4 table)")
            .arg(nodes)
            .arg(hidden)
            .arg(rows)
            .arg(cols));

    auto* tabs = new QTabWidget(&window);
    tabs->setObjectName(QStringLiteral("mainTabs"));
    tabs->addTab(makeGrid(nodes, tabs), QStringLiteral("Grid"));
    tabs->addTab(makeTable(rows, cols, tabs), QStringLiteral("Table"));
    tabs->addTab(makeStack(hidden, tabs), QStringLiteral("Stack"));
    window.setCentralWidget(tabs);

    auto* fileMenu = window.menuBar()->addMenu(QStringLiteral("&File"));
    fileMenu->setObjectName(QStringLiteral("fileMenu"));
    auto* quit = fileMenu->addAction(QStringLiteral("Quit"));
    quit->setObjectName(QStringLiteral("actionQuit"));

    window.statusBar()->showMessage(QStringLiteral("Ready"));
    window.resize(1200, 800);
    window.show();

    qt_commander::StartOptions options;
    options.name = QStringLiteral("large");
    options.rendezvous_dir = qEnvironmentVariable("QT_COMMANDER_RENDEZVOUS");
    const auto startResult = qt_commander::start(options);
    if (!startResult.ok)
        qWarning() << "qt-commander start failed:" << startResult.error;

    QTimer::singleShot(0, &window, [readyFile]() {
        if (readyFile.isEmpty())
            return;
        QFile file(readyFile);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            file.write("ready\n");
    });

    return app.exec();
}

