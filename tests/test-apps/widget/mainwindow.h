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
#include <QMouseEvent>

// Drag probe: records press/move/release positions so injected
// mouse-press/mouse-move/mouse-release sequences can be asserted from
// outside the process (style-independent; plain QWidget semantics).
class DragProbe : public QWidget {
    Q_OBJECT
    Q_PROPERTY(int pressX READ pressX)
    Q_PROPERTY(int moveX READ moveX)
    Q_PROPERTY(int releaseX READ releaseX)
    Q_PROPERTY(int moveCount READ moveCount)
public:
    explicit DragProbe(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("dragProbe"));
        setMinimumSize(220, 60);
        setStyleSheet(QStringLiteral("background:#FDEBD0;border:1px solid #bbb;"));
        // Without tracking, Qt drops button-less MouseMove events; with it,
        // hover moves are delivered too (drag moves carry the button anyway).
        setMouseTracking(true);
    }
    int pressX() const { return press_x_; }
    int moveX() const { return move_x_; }
    int releaseX() const { return release_x_; }
    int moveCount() const { return move_count_; }
protected:
    void mousePressEvent(QMouseEvent* e) override {
        press_x_ = e->pos().x();
        QWidget::mousePressEvent(e);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        move_x_ = e->pos().x();
        ++move_count_;
        QWidget::mouseMoveEvent(e);
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        release_x_ = e->pos().x();
        QWidget::mouseReleaseEvent(e);
    }
private:
    int press_x_ = -1;
    int move_x_ = -1;
    int release_x_ = -1;
    int move_count_ = 0;
};

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
    DragProbe* drag_probe_;

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
