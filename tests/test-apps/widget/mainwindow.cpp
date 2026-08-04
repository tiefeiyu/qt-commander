#include "mainwindow.h"
#include "test_dialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QMenuBar>
#include <QMenu>
#include <QStatusBar>
#include <QMessageBox>
#include <QStandardItemModel>
#include <QTreeWidgetItem>
#include <QListWidgetItem>
#include <QDateEdit>
#include <QTimeEdit>
#include <QDial>
#include <QProgressBar>
#include <QRadioButton>
#include <QCalendarWidget>
#include <QTextEdit>
#include <QDateTime>

WidgetTestWindow::WidgetTestWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("Qt Widget Test App"));
    resize(800, 600);
    setupUI();
}

void WidgetTestWindow::setupUI()
{
    auto* central = new QWidget(this);
    setCentralWidget(central);

    // mainLayout is wrapped by bodyRow (with the log panel) into
    // centralLayout at the end of setupUI.
    auto* mainLayout = new QVBoxLayout();

    // ---- Header ----
    auto* headerLabel = new QLabel(QStringLiteral("Qt Widget Test Application"));
    headerLabel->setObjectName(QStringLiteral("headerLabel"));
    QFont headerFont = headerLabel->font();
    headerFont.setPointSize(16);
    headerFont.setBold(true);
    headerLabel->setFont(headerFont);
    mainLayout->addWidget(headerLabel);

    // ---- Button row ----
    auto* buttonLayout = new QHBoxLayout();

    btn_ok_ = new QPushButton(QStringLiteral("OK"));
    btn_ok_->setObjectName(QStringLiteral("btnOk"));
    buttonLayout->addWidget(btn_ok_);

    btn_cancel_ = new QPushButton(QStringLiteral("Cancel"));
    btn_cancel_->setObjectName(QStringLiteral("btnCancel"));
    buttonLayout->addWidget(btn_cancel_);

    btn_dynamic_ = new QPushButton(QStringLiteral("Add/Remove"));
    btn_dynamic_->setObjectName(QStringLiteral("btnDynamic"));
    buttonLayout->addWidget(btn_dynamic_);

    btn_dialog_ = new QPushButton(QStringLiteral("Open Dialog"));
    btn_dialog_->setObjectName(QStringLiteral("btnDialog"));
    buttonLayout->addWidget(btn_dialog_);

    buttonLayout->addStretch();
    mainLayout->addLayout(buttonLayout);

    // ---- Input row ----
    auto* inputLayout = new QHBoxLayout();

    line_edit_ = new QLineEdit();
    line_edit_->setObjectName(QStringLiteral("lineEdit"));
    line_edit_->setPlaceholderText(QStringLiteral("Type something..."));
    inputLayout->addWidget(line_edit_);

    check_box_ = new QCheckBox(QStringLiteral("Enable feature"));
    check_box_->setObjectName(QStringLiteral("checkBox"));
    check_box_->setChecked(true);
    inputLayout->addWidget(check_box_);

    mainLayout->addLayout(inputLayout);

    // ---- Combo + Spin + Slider row ----
    auto* controlLayout = new QHBoxLayout();

    combo_box_ = new QComboBox();
    combo_box_->setObjectName(QStringLiteral("comboBox"));
    combo_box_->addItems({QStringLiteral("Option A"),
                          QStringLiteral("Option B"),
                          QStringLiteral("Option C")});
    controlLayout->addWidget(combo_box_);

    spin_box_ = new QSpinBox();
    spin_box_->setObjectName(QStringLiteral("spinBox"));
    spin_box_->setRange(0, 100);
    spin_box_->setValue(42);
    controlLayout->addWidget(spin_box_);

    slider_ = new QSlider(Qt::Horizontal);
    slider_->setObjectName(QStringLiteral("slider"));
    slider_->setRange(0, 100);
    slider_->setValue(50);
    controlLayout->addWidget(slider_);

    mainLayout->addLayout(controlLayout);

    // ---- Tab widget ----
    tab_widget_ = new QTabWidget();
    tab_widget_->setObjectName(QStringLiteral("tabWidget"));

    auto* tab1 = new QWidget();
    auto* tab1Layout = new QVBoxLayout(tab1);
    tab1Layout->addWidget(new QLabel(QStringLiteral("Tab 1 content")));
    tab_widget_->addTab(tab1, QStringLiteral("Tab 1"));

    auto* tab2 = new QWidget();
    auto* tab2Layout = new QVBoxLayout(tab2);
    tab2Layout->addWidget(new QLabel(QStringLiteral("Tab 2 content")));
    tab_widget_->addTab(tab2, QStringLiteral("Tab 2"));

    // Advanced tab: wide range of widget types for MCP interaction tests.
    setupAdvancedTab(tab_widget_);

    mainLayout->addWidget(tab_widget_);

    // ---- Operation log panel (right side) ----
    auto* logBox = new QGroupBox(QStringLiteral("Operation Log"));
    logBox->setObjectName(QStringLiteral("logBox"));
    auto* logLay = new QVBoxLayout(logBox);
    log_view_ = new QPlainTextEdit();
    log_view_->setObjectName(QStringLiteral("logView"));
    log_view_->setReadOnly(true);
    log_view_->setMaximumBlockCount(200);
    logLay->addWidget(log_view_);
    auto* rightCol = new QVBoxLayout();
    rightCol->addWidget(logBox, 1);
    rightCol->addStretch();

    auto* bodyRow = new QHBoxLayout();
    bodyRow->addLayout(mainLayout, 1);
    bodyRow->addLayout(rightCol, 0);
    auto* centralLayout = new QVBoxLayout(central);
    centralLayout->addLayout(bodyRow);
    resize(1150, 600);

    // ---- Menu bar ----
    auto* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    auto* actNew = fileMenu->addAction(QStringLiteral("&New"));
    actNew->setObjectName(QStringLiteral("actNew"));
    auto* actQuit = fileMenu->addAction(QStringLiteral("&Quit"));
    actQuit->setObjectName(QStringLiteral("actQuit"));
    QObject::connect(actQuit, &QAction::triggered, this, &QWidget::close);

    // ---- Wire widgets to the operation log ----
    QObject::connect(btn_cancel_, &QPushButton::clicked, this, [this]() {
        logOperation(QStringLiteral("Cancel button clicked"));
    });
    QObject::connect(check_box_, &QCheckBox::toggled, this, [this](bool on) {
        logOperation(QStringLiteral("Checkbox: ") +
                     (on ? QStringLiteral("checked") : QStringLiteral("unchecked")));
    });
    QObject::connect(combo_box_, &QComboBox::currentTextChanged, this, [this](const QString& t) {
        logOperation(QStringLiteral("Combo selection: ") + t);
    });
    QObject::connect(spin_box_, QOverload<int>::of(&QSpinBox::valueChanged),
                     this, [this](int v) {
        logOperation(QStringLiteral("Spinbox: ") + QString::number(v));
    });
    QObject::connect(slider_, &QSlider::valueChanged, this, [this](int v) {
        logOperation(QStringLiteral("Slider: ") + QString::number(v));
    });
    QObject::connect(tab_widget_, &QTabWidget::currentChanged, this, [this](int i) {
        logOperation(QStringLiteral("Switched to tab ") + QString::number(i + 1));
    });

    // ---- Dynamic container ----
    dynamic_container_ = new QWidget();
    dynamic_container_->setObjectName(QStringLiteral("dynamicContainer"));
    auto* dynLayout = new QVBoxLayout(dynamic_container_);
    dynLayout->addWidget(new QLabel(QStringLiteral("Dynamic items appear here")));
    mainLayout->addWidget(dynamic_container_);

    // ---- Bottom label ----
    label_ = new QLabel(QStringLiteral("Status: Ready"));
    label_->setObjectName(QStringLiteral("statusLabel"));
    mainLayout->addWidget(label_);

    // ---- Drag probe (draggable; placed manually so a drag visibly moves it
    //      instead of the layout snapping it back) ----
    drag_probe_ = new DragProbe(central);
    drag_probe_->setGeometry(9, 520, 850, 60);

    mainLayout->addStretch();

    // ---- Connect signals ----
    connect(btn_ok_, &QPushButton::clicked, this, &WidgetTestWindow::onOkClicked);
    connect(btn_dynamic_, &QPushButton::clicked, this, &WidgetTestWindow::onDynamicAddRemove);
    connect(btn_dialog_, &QPushButton::clicked, this, &WidgetTestWindow::onShowDialog);

    // Status bar
    statusBar()->showMessage(QStringLiteral("Ready"));
}

void WidgetTestWindow::setupAdvancedTab(QTabWidget* parent)
{
    auto* advanced = new QWidget();
    advanced->setObjectName(QStringLiteral("advancedTab"));
    auto* lay = new QVBoxLayout(advanced);

    // ---- Item views ----
    auto* viewsRow = new QHBoxLayout();

    table_view_ = new QTableView();
    table_view_->setObjectName(QStringLiteral("tableView"));
    auto* model = new QStandardItemModel(3, 3, table_view_);
    model->setHorizontalHeaderLabels(
        {QStringLiteral("Name"), QStringLiteral("Value"), QStringLiteral("Unit")});
    model->setItem(0, 0, new QStandardItem(QStringLiteral("Voltage")));
    model->setItem(0, 1, new QStandardItem(QStringLiteral("12")));
    model->setItem(0, 2, new QStandardItem(QStringLiteral("V")));
    model->setItem(1, 0, new QStandardItem(QStringLiteral("Current")));
    model->setItem(1, 1, new QStandardItem(QStringLiteral("2.5")));
    model->setItem(1, 2, new QStandardItem(QStringLiteral("A")));
    model->setItem(2, 0, new QStandardItem(QStringLiteral("Power")));
    model->setItem(2, 1, new QStandardItem(QStringLiteral("30")));
    model->setItem(2, 2, new QStandardItem(QStringLiteral("W")));
    table_view_->setModel(model);
    viewsRow->addWidget(table_view_, 2);

    tree_widget_ = new QTreeWidget();
    tree_widget_->setObjectName(QStringLiteral("treeWidget"));
    tree_widget_->setHeaderLabels({QStringLiteral("Item"), QStringLiteral("Qty")});
    auto* rootA = new QTreeWidgetItem(
        {QStringLiteral("Fruits"), QStringLiteral("")});
    rootA->addChild(new QTreeWidgetItem({QStringLiteral("Apple"), QStringLiteral("3")}));
    rootA->addChild(new QTreeWidgetItem({QStringLiteral("Banana"), QStringLiteral("5")}));
    auto* rootB = new QTreeWidgetItem(
        {QStringLiteral("Vegetables"), QStringLiteral("")});
    rootB->addChild(new QTreeWidgetItem({QStringLiteral("Carrot"), QStringLiteral("7")}));
    tree_widget_->addTopLevelItem(rootA);
    tree_widget_->addTopLevelItem(rootB);
    tree_widget_->expandAll();
    viewsRow->addWidget(tree_widget_, 2);

    list_widget_ = new QListWidget();
    list_widget_->setObjectName(QStringLiteral("listWidget"));
    list_widget_->addItem(QStringLiteral("Alpha"));
    list_widget_->addItem(QStringLiteral("Beta"));
    list_widget_->addItem(QStringLiteral("Gamma"));
    viewsRow->addWidget(list_widget_, 1);

    lay->addLayout(viewsRow, 2);

    // ---- Text edit ----
    text_edit_ = new QTextEdit();
    text_edit_->setObjectName(QStringLiteral("textEdit"));
    text_edit_->setPlainText(QStringLiteral("Initial document text"));
    lay->addWidget(text_edit_, 1);

    // ---- Calendar + date/time + dial + progress ----
    auto* calRow = new QHBoxLayout();

    calendar_ = new QCalendarWidget();
    calendar_->setObjectName(QStringLiteral("calendar"));
    calendar_->setSelectedDate(QDate(2026, 8, 3));
    calRow->addWidget(calendar_, 2);

    auto* rightCol = new QVBoxLayout();
    date_edit_ = new QDateEdit(QDate(2026, 8, 3));
    date_edit_->setObjectName(QStringLiteral("dateEdit"));
    rightCol->addWidget(date_edit_);
    auto* timeEdit = new QTimeEdit(QTime(9, 30, 0));
    timeEdit->setObjectName(QStringLiteral("timeEdit"));
    rightCol->addWidget(timeEdit);
    dial_ = new QDial();
    dial_->setObjectName(QStringLiteral("dial"));
    dial_->setRange(0, 100);
    dial_->setValue(40);
    rightCol->addWidget(dial_);
    progress_bar_ = new QProgressBar();
    progress_bar_->setObjectName(QStringLiteral("progressBar"));
    progress_bar_->setRange(0, 100);
    progress_bar_->setValue(25);
    rightCol->addWidget(progress_bar_);
    calRow->addLayout(rightCol, 1);

    lay->addLayout(calRow, 1);

    // ---- Radio buttons ----
    auto* radioRow = new QHBoxLayout();
    auto* radioGroup = new QGroupBox(QStringLiteral("Mode"));
    radioGroup->setObjectName(QStringLiteral("modeGroup"));
    auto* groupLay = new QHBoxLayout(radioGroup);
    radio_a_ = new QRadioButton(QStringLiteral("Auto"));
    radio_a_->setObjectName(QStringLiteral("radioAuto"));
    radio_a_->setChecked(true);
    radio_b_ = new QRadioButton(QStringLiteral("Manual"));
    radio_b_->setObjectName(QStringLiteral("radioManual"));
    groupLay->addWidget(radio_a_);
    groupLay->addWidget(radio_b_);
    radioRow->addWidget(radioGroup);
    radioRow->addStretch();
    lay->addLayout(radioRow);

    parent->addTab(advanced, QStringLiteral("Advanced"));
}

void WidgetTestWindow::logOperation(const QString& msg)
{
    log_view_->appendPlainText(QStringLiteral("[%1] %2")
                          .arg(QDateTime::currentDateTime()
                                   .toString(QStringLiteral("HH:mm:ss")),
                               msg));
}

void WidgetTestWindow::onOkClicked()
{
    label_->setText(QStringLiteral("Status: OK clicked! Text: %1")
                        .arg(line_edit_->text()));
    logOperation(QStringLiteral("OK clicked, input: ") + line_edit_->text());
}

void WidgetTestWindow::onDynamicAddRemove()
{
    static bool added = false;
    if (!added) {
        auto* newBtn = new QPushButton(QStringLiteral("Dynamic Button"));
        newBtn->setObjectName(QStringLiteral("dynamicBtn"));
        dynamic_container_->layout()->addWidget(newBtn);
        added = true;
        label_->setText(QStringLiteral("Status: Dynamic button added"));
        logOperation(QStringLiteral("Dynamic button added"));
    } else {
        auto* btn = dynamic_container_->findChild<QPushButton*>(
            QStringLiteral("dynamicBtn"));
        if (btn) {
            dynamic_container_->layout()->removeWidget(btn);
            btn->deleteLater();
        }
        added = false;
        label_->setText(QStringLiteral("Status: Dynamic button removed"));
        logOperation(QStringLiteral("Dynamic button removed"));
    }
}

void WidgetTestWindow::onShowDialog()
{
    logOperation(QStringLiteral("Dialog opened"));
    TestDialog dlg(this);
    dlg.exec();
}
