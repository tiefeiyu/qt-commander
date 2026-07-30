#include "test_dialog.h"
#include <QVBoxLayout>
#include <QLabel>

TestDialog::TestDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Test Dialog"));
    resize(400, 200);

    auto* layout = new QVBoxLayout(this);

    auto* label = new QLabel(QStringLiteral("This is a modal test dialog"));
    layout->addWidget(label);

    line_edit_ = new QLineEdit();
    line_edit_->setObjectName(QStringLiteral("dialogLineEdit"));
    line_edit_->setPlaceholderText(QStringLiteral("Enter text in dialog..."));
    layout->addWidget(line_edit_);

    btn_ok_ = new QPushButton(QStringLiteral("OK"));
    btn_ok_->setObjectName(QStringLiteral("dialogOkBtn"));
    layout->addWidget(btn_ok_);

    btn_cancel_ = new QPushButton(QStringLiteral("Cancel"));
    btn_cancel_->setObjectName(QStringLiteral("dialogCancelBtn"));
    layout->addWidget(btn_cancel_);

    connect(btn_ok_, &QPushButton::clicked, this, &QDialog::accept);
    connect(btn_cancel_, &QPushButton::clicked, this, &QDialog::reject);
}

QString TestDialog::lineEditText() const
{
    return line_edit_->text();
}
