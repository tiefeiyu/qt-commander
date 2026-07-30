#pragma once
#include <QDialog>
#include <QPushButton>
#include <QLineEdit>

class TestDialog : public QDialog {
    Q_OBJECT
public:
    explicit TestDialog(QWidget* parent = nullptr);
    QString lineEditText() const;
private:
    QPushButton* btn_ok_;
    QPushButton* btn_cancel_;
    QLineEdit* line_edit_;
};
