// QWidget occlusion test app for qt-commander integration tests.
// Manually-positioned overlapping children: QWidget has no z property --
// the widget stack is creation order, so later-created siblings paint
// above earlier ones (mirrors QML's equal-z rule).
//
// Expected prune outcome:
//   occlRed    partially covered by occlGreen -> kept, visible_ratio < 1
//   occlGreen  later sibling                 -> kept, ratio 1
//   occlBlue   fully covered by occlYellow   -> REMOVED
//   occlYellow same geometry, later          -> kept
//   occlBtnRoot covered by its own bg child  -> REMOVED, bg kept
//   occlHidden invisible                     -> never in snapshot
#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QLabel>

#if defined(_WIN32) && defined(_MSC_VER)
#include <windows.h>
#include <crtdbg.h>
#endif

class OcclusionWindow : public QWidget {
    Q_OBJECT
public:
    explicit OcclusionWindow(QWidget* parent = nullptr) : QWidget(parent)
    {
        setWindowTitle(QStringLiteral("Widget Occlusion Test"));
        resize(640, 420);
        setObjectName(QStringLiteral("occlRoot"));

        auto* lay = new QVBoxLayout(this);
        auto* title = new QLabel(QStringLiteral("Widget occlusion test"));
        title->setObjectName(QStringLiteral("occlStatus"));
        lay->addWidget(title);
        lay->addStretch();

        auto* occl = new QWidget(this);
        occl->setObjectName(QStringLiteral("occlContainer"));
        occl->setGeometry(0, 40, 640, 380);
        occl->setStyleSheet(
            QStringLiteral("background-color: #e8e8e8;"));

        // Partial overlap: green (later) covers part of red (earlier).
        auto* red = new QWidget(occl);
        red->setObjectName(QStringLiteral("occlRed"));
        red->setGeometry(10, 10, 120, 80);
        red->setStyleSheet(QStringLiteral("background-color: red;"));

        auto* green = new QWidget(occl);
        green->setObjectName(QStringLiteral("occlGreen"));
        green->setGeometry(40, 30, 120, 80);
        green->setStyleSheet(QStringLiteral("background-color: green;"));

        // Full overlap: yellow (later, same geometry) covers blue.
        auto* blue = new QWidget(occl);
        blue->setObjectName(QStringLiteral("occlBlue"));
        blue->setGeometry(180, 10, 120, 80);
        blue->setStyleSheet(QStringLiteral("background-color: blue;"));

        auto* yellow = new QWidget(occl);
        yellow->setObjectName(QStringLiteral("occlYellow"));
        yellow->setGeometry(180, 10, 120, 80);
        yellow->setStyleSheet(QStringLiteral("background-color: yellow;"));

        // Button-root pattern: a container whose full-size child paints
        // over it.  The root is fully covered -> pruned, bg kept.
        auto* btnRoot = new QWidget(occl);
        btnRoot->setObjectName(QStringLiteral("occlBtnRoot"));
        btnRoot->setGeometry(10, 130, 40, 40);
        auto* btnBg = new QWidget(btnRoot);
        btnBg->setObjectName(QStringLiteral("occlBtnBg"));
        btnBg->setGeometry(0, 0, 40, 40);
        btnBg->setStyleSheet(QStringLiteral("background-color: #4CAF50;"));

        // Hidden widget: must never appear, must never occlude.
        auto* hidden = new QWidget(occl);
        hidden->setObjectName(QStringLiteral("occlHidden"));
        hidden->setGeometry(270, 130, 150, 100);
        hidden->setStyleSheet(QStringLiteral("background-color: black;"));
        hidden->hide();
    }
};

int main(int argc, char* argv[])
{
#if defined(_WIN32) && defined(_MSC_VER)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    QApplication app(argc, argv);

    OcclusionWindow win;
    win.show();

    return app.exec();
}

#include "main_widget.moc"
