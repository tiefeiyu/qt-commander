#pragma once
#include <QMainWindow>
#include <QPushButton>
#include <QLineEdit>
#include <QCheckBox>
#include <QComboBox>
#include <QTabWidget>
#include <QLabel>
#include <QSpinBox>
#include <QSlider>
#include <QDialog>

class WidgetTestWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit WidgetTestWindow(QWidget* parent = nullptr);

private slots:
    void onOkClicked();
    void onDynamicAddRemove();
    void onShowDialog();

private:
    void setupUI();
    QPushButton* btn_ok_;
    QPushButton* btn_cancel_;
    QPushButton* btn_dynamic_;
    QPushButton* btn_dialog_;
    QLineEdit* line_edit_;
    QCheckBox* check_box_;
    QComboBox* combo_box_;
    QTabWidget* tab_widget_;
    QLabel* label_;
    QSpinBox* spin_box_;
    QSlider* slider_;
    QWidget* dynamic_container_;
};
