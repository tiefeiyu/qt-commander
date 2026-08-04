#pragma once
#include <QtGlobal>
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
#include <QPainter>
#include <QPoint>

// Drag probe: a draggable widget.  Press inside it, then move -- the probe
// follows the mouse (like dragging an icon) so injected press/move/release
// sequences have a visible effect: the widget's geometry changes and the
// on-screen text reports the drag distance.  Style-independent (plain
// QWidget semantics) so drag assertions work with any Windows style.
class DragProbe : public QWidget {
    Q_OBJECT
    Q_PROPERTY(int pressX READ pressX)
    Q_PROPERTY(int moveX READ moveX)
    Q_PROPERTY(int releaseX READ releaseX)
    Q_PROPERTY(int moveCount READ moveCount)
    Q_PROPERTY(int dragDX READ dragDX)
    Q_PROPERTY(int dragDY READ dragDY)
public:
    explicit DragProbe(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("dragProbe"));
        setMinimumSize(220, 60);
        // Without tracking, Qt drops button-less MouseMove events; with it,
        // hover moves are delivered too (drag moves carry the button anyway).
        setMouseTracking(true);
    }
    int pressX() const { return press_x_; }
    int moveX() const { return move_x_; }
    int releaseX() const { return release_x_; }
    int moveCount() const { return move_count_; }
    int dragDX() const { return x() - drag_start_x_; }
    int dragDY() const { return y() - drag_start_y_; }
protected:
    // QMouseEvent::globalPosition() is Qt6-only; Qt5.15 has globalPos().
    static QPoint eventGlobalPos(const QMouseEvent* e) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        return e->globalPosition().toPoint();
#else
        return e->globalPos();
#endif
    }
    void mousePressEvent(QMouseEvent* e) override {
        press_x_ = e->pos().x();
        drag_start_x_ = x();
        drag_start_y_ = y();
        // Where in the probe the press landed (probe-local); stays fixed
        // for the whole drag.
        grab_offset_ = e->pos();
        QWidget::mousePressEvent(e);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        move_x_ = e->pos().x();
        ++move_count_;
        // Follow the mouse while the left button is held.  move() takes
        // parent coordinates: mapFromGlobal gives the pointer in *this*
        // widget's coordinates, so add pos() to translate it into the
        // parent's, then drop the fixed grab point -- the probe moves by
        // exactly the pointer delta (real drag semantics).
        if (e->buttons() & Qt::LeftButton)
            move(mapFromGlobal(eventGlobalPos(e)) + pos() - grab_offset_);
        QWidget::mouseMoveEvent(e);
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        release_x_ = e->pos().x();
        QWidget::mouseReleaseEvent(e);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(0xFD, 0xEB, 0xD0));
        p.setPen(QColor(0x88, 0x50, 0x00));
        p.drawText(rect(), Qt::AlignCenter,
                   QStringLiteral("DragProbe  dx=%1  dy=%2   press=%3  move=%4  release=%5")
                       .arg(dragDX()).arg(dragDY()).arg(press_x_)
                       .arg(move_x_).arg(release_x_));
    }
private:
    int press_x_ = -1;
    int move_x_ = -1;
    int release_x_ = -1;
    int move_count_ = 0;
    int drag_start_x_ = 0;
    int drag_start_y_ = 0;
    QPoint grab_offset_;
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
