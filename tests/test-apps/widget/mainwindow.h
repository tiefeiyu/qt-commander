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
#include <QTableView>
#include <QTreeWidget>
#include <QListWidget>
#include <QTextEdit>
#include <QCalendarWidget>
#include <QDateEdit>
#include <QDial>
#include <QProgressBar>
#include <QRadioButton>
#include <QPlainTextEdit>
#include <QTextEdit>

class WidgetTestWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit WidgetTestWindow(QWidget* parent = nullptr);

private slots:
    void onOkClicked();
    void onDynamicAddRemove();
    void onShowDialog();
    void logOperation(const QString& msg);

private:
    void setupUI();
    void setupAdvancedTab(QTabWidget* parent);

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

    QPlainTextEdit* log_view_;

    // Advanced tab widgets
    QTableView* table_view_;
    QTreeWidget* tree_widget_;
    QListWidget* list_widget_;
    QTextEdit* text_edit_;
    QCalendarWidget* calendar_;
    QDateEdit* date_edit_;
    QDial* dial_;
    QProgressBar* progress_bar_;
    QRadioButton* radio_a_;
    QRadioButton* radio_b_;
};
