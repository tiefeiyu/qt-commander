#include "mainwindow.h"
#include "test_dialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QMenuBar>
#include <QStatusBar>
#include <QMessageBox>

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

    auto* mainLayout = new QVBoxLayout(central);

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

    mainLayout->addWidget(tab_widget_);

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

    mainLayout->addStretch();

    // ---- Connect signals ----
    connect(btn_ok_, &QPushButton::clicked, this, &WidgetTestWindow::onOkClicked);
    connect(btn_dynamic_, &QPushButton::clicked, this, &WidgetTestWindow::onDynamicAddRemove);
    connect(btn_dialog_, &QPushButton::clicked, this, &WidgetTestWindow::onShowDialog);

    // Status bar
    statusBar()->showMessage(QStringLiteral("Ready"));
}

void WidgetTestWindow::onOkClicked()
{
    label_->setText(QStringLiteral("Status: OK clicked! Text: %1")
                        .arg(line_edit_->text()));
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
    } else {
        auto* btn = dynamic_container_->findChild<QPushButton*>(
            QStringLiteral("dynamicBtn"));
        if (btn) {
            dynamic_container_->layout()->removeWidget(btn);
            btn->deleteLater();
        }
        added = false;
        label_->setText(QStringLiteral("Status: Dynamic button removed"));
    }
}

void WidgetTestWindow::onShowDialog()
{
    TestDialog dlg(this);
    dlg.exec();
}
