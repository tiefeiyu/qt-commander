#pragma once

// A label that records what the runtime actually dispatched to it: the button,
// the held-buttons mask, the modifiers, the event type, hover transitions and
// whether a context menu arrived. The e2e tests read `text` back over RPC, so
// every assertion is about what the widget really received.
//
// Deliberately not a Q_OBJECT: overrides of virtuals need no moc, which keeps
// this header usable from both the injected test app and ad-hoc probes.

#include <QContextMenuEvent>
#include <QEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QWheelEvent>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QEnterEvent>
#endif

class MouseProbe : public QLabel
{
public:
    explicit MouseProbe(QWidget* parent = nullptr)
        : QLabel(QStringLiteral("none"), parent)
    {
        setMouseTracking(true);
        setAlignment(Qt::AlignCenter);
        setMinimumHeight(56);
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        record(QStringLiteral("press"), event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        record(QStringLiteral("release"), event);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        record(QStringLiteral("dblclick"), event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (event->buttons() == Qt::NoButton) {
            ++moves_;
            setText(QStringLiteral("move %1").arg(moves_));
            return;
        }
        record(QStringLiteral("move"), event);
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    void enterEvent(QEnterEvent*) override
#else
    void enterEvent(QEvent*) override
#endif
    {
        ++enters_;
        setText(QStringLiteral("enter %1").arg(enters_));
    }

    void leaveEvent(QEvent*) override
    {
        ++leaves_;
        setText(QStringLiteral("leave %1").arg(leaves_));
    }

    void contextMenuEvent(QContextMenuEvent* event) override
    {
        ++menus_;
        setText(QStringLiteral("menu %1 at %2,%3 mods=%4")
                    .arg(menus_)
                    .arg(event->pos().x())
                    .arg(event->pos().y())
                    .arg(modifierNames(event->modifiers())));
        event->accept();
    }

    void wheelEvent(QWheelEvent* event) override
    {
        setText(QStringLiteral("wheel %1,%2 mods=%3")
                    .arg(event->angleDelta().x())
                    .arg(event->angleDelta().y())
                    .arg(modifierNames(event->modifiers())));
    }

private:
    static QString buttonName(Qt::MouseButton button)
    {
        switch (button) {
        case Qt::LeftButton:
            return QStringLiteral("left");
        case Qt::RightButton:
            return QStringLiteral("right");
        case Qt::MiddleButton:
            return QStringLiteral("middle");
        case Qt::BackButton:
            return QStringLiteral("back");
        case Qt::ForwardButton:
            return QStringLiteral("forward");
        default:
            return QStringLiteral("none");
        }
    }

    static QString heldNames(Qt::MouseButtons buttons)
    {
        if (buttons == Qt::NoButton)
            return QStringLiteral("none");
        QStringList names;
        const Qt::MouseButton all[] = {Qt::LeftButton, Qt::RightButton,
                                       Qt::MiddleButton, Qt::BackButton,
                                       Qt::ForwardButton};
        for (Qt::MouseButton candidate : all) {
            if (buttons & candidate)
                names.append(buttonName(candidate));
        }
        return names.join(QLatin1Char('+'));
    }

    static QString modifierNames(Qt::KeyboardModifiers modifiers)
    {
        if (modifiers == Qt::NoModifier)
            return QStringLiteral("none");
        QStringList names;
        if (modifiers & Qt::ControlModifier)
            names.append(QStringLiteral("ctrl"));
        if (modifiers & Qt::ShiftModifier)
            names.append(QStringLiteral("shift"));
        if (modifiers & Qt::AltModifier)
            names.append(QStringLiteral("alt"));
        if (modifiers & Qt::MetaModifier)
            names.append(QStringLiteral("meta"));
        return names.join(QLatin1Char('+'));
    }

    void record(const QString& type, QMouseEvent* event)
    {
        setText(QStringLiteral("%1 btn=%2 buttons=%3 mods=%4")
                    .arg(type, buttonName(event->button()),
                         heldNames(event->buttons()),
                         modifierNames(event->modifiers())));
    }

    int moves_ = 0;
    int enters_ = 0;
    int leaves_ = 0;
    int menus_ = 0;
};

