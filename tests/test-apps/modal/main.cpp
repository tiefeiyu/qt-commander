// Modal/async regression target for the qt-commander task queue, and a hostile
// application for the input actions: it tears widgets down from inside the very
// keystroke that was delivered to them, which is what a real settings dialog
// does when Enter accepts it and drops the page (and the QLineEdit on it).
// A second synthetic event aimed at the destroyed object must not reach it.
//
// Modes:
//   click-modal     : a button opens a modal QDialog via exec()
//   nested-modal    : A opens B, both via exec() (two nested event loops)
//   delete-on-close : the modal has WA_DeleteOnClose (dangling-pointer guard)
//   type-submit-destroys-editor : Enter deletes the editor page (and the
//                      QLineEdit that received the Return) and accepts the
//                      dialog - the Qt Creator Options/dialog case
//   type-submit-destroys-child  : Enter deletes only the editable child, the
//                      container target survives
//   type-submit-destroys-container : Enter deletes the container and its
//                      editor together (both anchors die)
//   type-char-release-destroys-child : the child dies on the KeyRelease, after
//                      the character was inserted; the container survives
//   type-texteditors : QTextEdit / QPlainTextEdit targets (typing + capture)
//   type-submit-rebuilds-editor : Enter drops the editor page and rebuilds it
//                      in place, so the old id must go stale and the new page
//                      must be addressable
//   type-submit-closes-plain    : Enter accepts the dialog, nothing is deleted
//   type-enter-keeps-open       : Enter only updates a label
//   type-char-destroys-editor   : typing 'X' tears the editor down
//   hover-destroys-target       : the Enter event deletes the widget it was
//                      delivered to, before the HoverEnter that follows
#include <QApplication>
#include <QDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

#include <functional>

#include "qt_commander/agent.h"

namespace {

// ---- destructive keystroke hooks -----------------------------------------
//
// The deletion runs after the widget finished handling the event but still
// inside QApplication::notify(). That is the window in which qt-commander
// dispatches its next synthetic event, so the fixture must be able to destroy
// the receiver there without touching the freed object itself afterwards.
//
// This point is a deliberate approximation: a real app may delete the widget
// from inside the widget's own handler, which cannot be reproduced here without
// making the fixture itself undefined. What the fixture pins is exactly the
// sender-visible contract - the event was delivered, the receiver is now gone,
// and the next event must not be sent - which is the shape of the production
// bug.

struct TeardownHook
{
    QPointer<QWidget> watch;
    QEvent::Type eventType = QEvent::KeyPress;
    int key = 0;                 // Qt::Key_Return, or 0 for "any character"
    QChar character;              // used when key == 0
    std::function<void()> teardown;
};

QVector<TeardownHook> g_hooks;
QString g_lastEditorText;

QString editorText(QWidget* widget)
{
    if (auto* line = qobject_cast<QLineEdit*>(widget))
        return line->text();
    if (auto* line = widget->findChild<QLineEdit*>())
        return line->text();
    return QString();
}

void runHooks(QObject* receiver, QEvent* event)
{
    auto* keyEvent = event->type() == QEvent::KeyPress ||
                             event->type() == QEvent::KeyRelease
                         ? static_cast<QKeyEvent*>(event)
                         : nullptr;
    for (int index = 0; index < g_hooks.size(); ++index) {
        TeardownHook& hook = g_hooks[index];
        if (hook.watch.isNull() || hook.watch.data() != receiver)
            continue;
        if (event->type() != hook.eventType)
            continue;
        if (keyEvent) {
            const bool matches =
                hook.key != 0
                    ? keyEvent->key() == hook.key
                    : (!keyEvent->text().isEmpty() &&
                       keyEvent->text().at(0) == hook.character);
            if (!matches)
                continue;
        }
        g_lastEditorText = editorText(hook.watch.data());
        const std::function<void()> teardown = hook.teardown;
        g_hooks.removeAt(index);
        teardown(); // may delete the receiver: nothing below touches it
        return;
    }
}

class ProbeApplication : public QApplication
{
public:
    ProbeApplication(int& argc, char** argv) : QApplication(argc, argv) {}

    bool notify(QObject* receiver, QEvent* event) override
    {
        const bool handled = QApplication::notify(receiver, event);
        runHooks(receiver, event);
        return handled;
    }
};

// ---- window furniture -----------------------------------------------------

QLabel* addLabel(QWidget* parent, const QString& name, const QString& text)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName(name);
    return label;
}

QPushButton* addButton(QWidget* parent, const QString& name,
                       const QString& text)
{
    auto* button = new QPushButton(text, parent);
    button->setObjectName(name);
    return button;
}

void addEditorPage(QDialog* dialog, const QString& pageName,
                   const QString& editName)
{
    auto* page = new QWidget(dialog);
    page->setObjectName(pageName);
    auto* layout = new QVBoxLayout(page);
    auto* edit = new QLineEdit(QStringLiteral("127.0.0.1"), page);
    edit->setObjectName(editName);
    auto* status = addLabel(page, QStringLiteral("editorStatus"),
                            QStringLiteral("text:127.0.0.1"));
    layout->addWidget(edit);
    layout->addWidget(status);
    QObject::connect(edit, &QLineEdit::textChanged, status,
                     [status](const QString& text) {
                         status->setText(QStringLiteral("text:") + text);
                     });
    if (auto* box = qobject_cast<QVBoxLayout*>(dialog->layout()))
        box->insertWidget(0, page);
}

QDialog* makeEditorDialog(QWidget* parent)
{
    auto* dialog = new QDialog(parent);
    dialog->setObjectName(QStringLiteral("editorDialog"));
    dialog->setWindowTitle(QStringLiteral("Editor"));
    dialog->setModal(true);
    auto* layout = new QVBoxLayout(dialog);
    addEditorPage(dialog, QStringLiteral("editorPage"),
                  QStringLiteral("lineEditHost"));
    auto* ok = addButton(dialog, QStringLiteral("btnEditorOk"),
                         QStringLiteral("OK"));
    ok->setDefault(true);
    QObject::connect(ok, &QPushButton::clicked, dialog, &QDialog::accept);
    layout->addWidget(ok);
    dialog->resize(360, 180);
    return dialog;
}

QDialog* makeDialog(QWidget* parent, const QString& className,
                    const QString& title, const QString& buttonName,
                    const QString& buttonText)
{
    auto* dialog = new QDialog(parent);
    dialog->setObjectName(className);
    dialog->setWindowTitle(title);
    dialog->setModal(true);
    auto* layout = new QVBoxLayout(dialog);
    auto* close = new QPushButton(buttonText, dialog);
    close->setObjectName(buttonName);
    layout->addWidget(close);
    QObject::connect(close, &QPushButton::clicked, dialog, &QDialog::accept);
    dialog->resize(220, 100);
    return dialog;
}

void installReturnTeardown(QLineEdit* edit, std::function<void()> teardown)
{
    TeardownHook hook;
    hook.watch = edit;
    hook.key = Qt::Key_Return;
    hook.teardown = std::move(teardown);
    g_hooks.append(hook);
}

void installCharacterTeardown(QLineEdit* edit, QChar character,
                              std::function<void()> teardown)
{
    TeardownHook hook;
    hook.watch = edit;
    hook.character = character;
    hook.teardown = std::move(teardown);
    g_hooks.append(hook);
}

// Same, but the widget dies while handling the KeyRelease, after the KeyPress
// already inserted the character.
void installReleaseTeardown(QLineEdit* edit, QChar character,
                            std::function<void()> teardown)
{
    TeardownHook hook;
    hook.watch = edit;
    hook.eventType = QEvent::KeyRelease;
    hook.character = character;
    hook.teardown = std::move(teardown);
    g_hooks.append(hook);
}

// Rebuilds the editor page on every Enter, so the old id goes stale each time.
void installRebuildTeardown(QDialog* dialog)
{
    auto* edit = dialog->findChild<QLineEdit*>(QStringLiteral("lineEditHost"));
    if (!edit)
        return;
    installReturnTeardown(edit, [dialog]() {
        for (const QString& page : {QStringLiteral("editorPage"),
                                    QStringLiteral("editorPageRebuilt")}) {
            if (auto* found = dialog->findChild<QWidget*>(page))
                delete found;
        }
        addEditorPage(dialog, QStringLiteral("editorPageRebuilt"),
                      QStringLiteral("lineEditHost"));
        installRebuildTeardown(dialog);
    });
}

} // namespace

int main(int argc, char** argv)
{
    ProbeApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qt-modal-test"));

    const QStringList args = app.arguments();
    const int index = args.indexOf(QStringLiteral("--mode"));
    const QString mode = (index >= 0 && index + 1 < args.size())
                             ? args.at(index + 1)
                             : QStringLiteral("click-modal");

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("Qt Modal Test: %1").arg(mode));
    auto* central = new QWidget(&window);
    auto* layout = new QVBoxLayout(central);
    auto* status = addLabel(central, QStringLiteral("statusLabel"),
                            QStringLiteral("idle"));
    auto* fast = addButton(central, QStringLiteral("btnFast"),
                           QStringLiteral("Fast"));
    QObject::connect(fast, &QPushButton::clicked, status,
                     [status]() { status->setText(QStringLiteral("clicked")); });
    layout->addWidget(status);
    layout->addWidget(fast);
    window.setCentralWidget(central);
    window.resize(420, 220);
    window.show();

    if (mode == QStringLiteral("click-modal")) {
        QDialog* dialog = makeDialog(&window, QStringLiteral("probeDialog"),
                                     QStringLiteral("Probe"),
                                     QStringLiteral("btnCloseDialog"),
                                     QStringLiteral("Close"));
        auto* open = addButton(central, QStringLiteral("btnOpenModal"),
                               QStringLiteral("Open modal"));
        layout->addWidget(open);
        QObject::connect(open, &QPushButton::clicked, dialog, &QDialog::exec);
    } else if (mode == QStringLiteral("nested-modal")) {
        QDialog* dialogA = makeDialog(&window, QStringLiteral("dialogA"),
                                      QStringLiteral("A"),
                                      QStringLiteral("btnCloseA"),
                                      QStringLiteral("Close A"));
        auto* openB = new QPushButton(QStringLiteral("Open B"), dialogA);
        openB->setObjectName(QStringLiteral("btnOpenB"));
        dialogA->layout()->addWidget(openB);
        QDialog* dialogB = makeDialog(dialogA, QStringLiteral("dialogB"),
                                      QStringLiteral("B"),
                                      QStringLiteral("btnCloseB"),
                                      QStringLiteral("Close B"));
        QObject::connect(openB, &QPushButton::clicked, dialogB, &QDialog::exec);
        auto* openA = addButton(central, QStringLiteral("btnOpenA"),
                                QStringLiteral("Open A"));
        layout->addWidget(openA);
        QObject::connect(openA, &QPushButton::clicked, dialogA, &QDialog::exec);
    } else if (mode == QStringLiteral("delete-on-close")) {
        QDialog* dialog = makeDialog(&window, QStringLiteral("doomedDialog"),
                                     QStringLiteral("Doomed"),
                                     QStringLiteral("btnCloseDoomed"),
                                     QStringLiteral("Close"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        auto* open = addButton(central, QStringLiteral("btnOpenDoomed"),
                               QStringLiteral("Open doomed"));
        layout->addWidget(open);
        QObject::connect(open, &QPushButton::clicked, dialog, &QDialog::exec);
    } else if (mode == QStringLiteral("type-submit-destroys-editor") ||
               mode == QStringLiteral("type-submit-closes-plain") ||
               mode == QStringLiteral("type-submit-rebuilds-editor")) {
        QDialog* dialog = makeEditorDialog(&window);
        auto* open = addButton(central, QStringLiteral("btnOpenEditor"),
                               QStringLiteral("Open editor"));
        layout->addWidget(open);
        QObject::connect(open, &QPushButton::clicked, dialog, &QDialog::exec);
        auto* edit = dialog->findChild<QLineEdit*>(QStringLiteral("lineEditHost"));
        if (mode == QStringLiteral("type-submit-destroys-editor")) {
            installReturnTeardown(edit, [dialog, edit]() {
                delete edit->parentWidget(); // the page owns the QLineEdit
                dialog->accept();
            });
        } else if (mode == QStringLiteral("type-submit-closes-plain")) {
            QObject::connect(edit, &QLineEdit::returnPressed, dialog,
                             &QDialog::accept);
        } else {
            installRebuildTeardown(dialog);
        }
    } else if (mode == QStringLiteral("type-submit-destroys-child")) {
        auto* container = new QWidget(central);
        container->setObjectName(QStringLiteral("containerHost"));
        auto* containerLayout = new QVBoxLayout(container);
        auto* edit = new QLineEdit(QStringLiteral("127.0.0.1"), container);
        edit->setObjectName(QStringLiteral("lineEditHost"));
        containerLayout->addWidget(edit);
        layout->addWidget(container);
        installReturnTeardown(edit, [edit]() { delete edit; });
    } else if (mode == QStringLiteral("type-submit-destroys-container")) {
        // Enter takes the whole container (and the editor with it), so the
        // addressed target and the keystroke receiver die together.
        auto* container = new QWidget(central);
        container->setObjectName(QStringLiteral("containerHost"));
        auto* containerLayout = new QVBoxLayout(container);
        auto* edit = new QLineEdit(QStringLiteral("127.0.0.1"), container);
        edit->setObjectName(QStringLiteral("lineEditHost"));
        containerLayout->addWidget(edit);
        layout->addWidget(container);
        installReturnTeardown(edit, [container]() { delete container; });
    } else if (mode == QStringLiteral("type-char-release-destroys-child")) {
        // The child dies on the KeyRelease, after the KeyPress inserted 'X';
        // the addressed container stays alive, so the release is the event that
        // must not be followed by more input.
        auto* container = new QWidget(central);
        container->setObjectName(QStringLiteral("containerHost"));
        auto* containerLayout = new QVBoxLayout(container);
        auto* edit = new QLineEdit(QStringLiteral("127.0.0.1"), container);
        edit->setObjectName(QStringLiteral("lineEditHost"));
        containerLayout->addWidget(edit);
        layout->addWidget(container);
        installReleaseTeardown(edit, QChar(QLatin1Char('X')),
                               [edit]() { delete edit; });
    } else if (mode == QStringLiteral("type-texteditors")) {
        auto* plain = new QPlainTextEdit(central);
        plain->setObjectName(QStringLiteral("plainEditorHost"));
        plain->setPlainText(QStringLiteral("seed"));
        auto* rich = new QTextEdit(central);
        rich->setObjectName(QStringLiteral("richEditorHost"));
        rich->setPlainText(QStringLiteral("seed"));
        layout->addWidget(plain);
        layout->addWidget(rich);
    } else if (mode == QStringLiteral("type-enter-keeps-open")) {
        auto* edit = new QLineEdit(QStringLiteral("127.0.0.1"), central);
        edit->setObjectName(QStringLiteral("lineEditHost"));
        auto* submitted = addLabel(central, QStringLiteral("submittedLabel"),
                                   QStringLiteral("submitted:none"));
        auto* notEditable = addLabel(central, QStringLiteral("notEditableLabel"),
                                     QStringLiteral("label"));
        layout->addWidget(edit);
        layout->addWidget(submitted);
        layout->addWidget(notEditable);
        QObject::connect(edit, &QLineEdit::returnPressed, submitted,
                         [edit, submitted]() {
                             submitted->setText(QStringLiteral("submitted:") +
                                                edit->text());
                         });
    } else if (mode == QStringLiteral("hover-destroys-target")) {
        // The Enter event deletes the widget that received it, like a hover
        // handler that closes the popup it belongs to; the HoverEnter that the
        // same action sends next must not reach the freed widget.
        auto* host = new QWidget(central);
        host->setObjectName(QStringLiteral("hoverHost"));
        host->setFixedSize(200, 60);
        layout->addWidget(host);
        TeardownHook hook;
        hook.watch = host;
        hook.eventType = QEvent::Enter;
        hook.teardown = [host]() { delete host; };
        g_hooks.append(hook);
    } else if (mode == QStringLiteral("type-char-destroys-editor")) {
        auto* edit = new QLineEdit(QStringLiteral("127.0.0.1"), central);
        edit->setObjectName(QStringLiteral("lineEditHost"));
        auto* gone = addLabel(central, QStringLiteral("editorGoneLabel"),
                              QStringLiteral("editor:alive"));
        layout->addWidget(edit);
        layout->addWidget(gone);
        installCharacterTeardown(edit, QChar(QLatin1Char('X')),
                                 [edit, gone]() {
                                     gone->setText(QStringLiteral("editor:gone ") +
                                                   g_lastEditorText);
                                     delete edit;
                                 });
    }

    qt_commander::StartOptions options;
    options.name = QStringLiteral("qt-modal-test-%1").arg(mode);
    const QByteArray rendezvous = qgetenv("QT_COMMANDER_RENDEZVOUS");
    if (!rendezvous.isEmpty())
        options.rendezvous_dir = QString::fromLocal8Bit(rendezvous);
    qt_commander::start(options);
    return app.exec();
}
