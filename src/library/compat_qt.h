#pragma once

// ---------------------------------------------------------------------------
// qt-commander -- Qt5 / Qt6 compatibility layer
// ---------------------------------------------------------------------------
// Include this file FIRST in any .cpp that needs Qt APIs.  It defines
// preprocessor macros that paper over API differences between Qt 5 and
// Qt 6 so the rest of the library source can be written once.
//
// Guidelines for adding new macros:
//   - Keep the macro name descriptive and prefixed with QT_COMMANDER_.
//   - Provide a brief comment showing the Qt5 vs Qt6 API difference.
//   - Test both Qt5 and Qt6 builds after every change.
// ---------------------------------------------------------------------------

#include <QtCore/qglobal.h>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#  define QT_COMMANDER_QT6
#endif

// ========================== Core / QtGlobal ==========================

// QStringRef (Qt5) was removed in Qt6; replaced by QStringView.
// Use QT_COMMANDER_STRING_VIEW where you previously used QStringRef.
#ifdef QT_COMMANDER_QT6
#  define QT_COMMANDER_STRING_VIEW   QStringView
#else
#  define QT_COMMANDER_STRING_VIEW   QStringRef
#endif

// QVariant::Type enum was removed in Qt6.
// Use QMetaType::Type (int) instead for type checks.
#ifdef QT_COMMANDER_QT6
#  define QT_COMMANDER_VARIANT_TYPE  int
#else
#  define QT_COMMANDER_VARIANT_TYPE  QVariant::Type
#endif

// QString::SkipEmptyParts (Qt5)  ->  Qt::SkipEmptyParts (Qt6)
#ifdef QT_COMMANDER_QT6
#  define QT_COMMANDER_SKIP_EMPTY_PARTS  Qt::SkipEmptyParts
#else
#  define QT_COMMANDER_SKIP_EMPTY_PARTS  QString::SkipEmptyParts
#endif

// QSet<T>::toList() (Qt5)  ->  QSet<T>::values() (Qt6)
#ifdef QT_COMMANDER_QT6
#  define QT_COMMANDER_SET_TO_LIST(set)  (set).values()
#else
#  define QT_COMMANDER_SET_TO_LIST(set)  (set).toList()
#endif

// QAlignedStorage (Qt5 internal) -> QTypeInfo / std::aligned_storage (Qt6).
// Not commonly used in user code; provided for completeness.

// qMin/qMax with Qt numeric types:  In Qt6 the global overloads were
// tightened;  use the std:: versions instead in new code.

// ======================== Text / String =========================

// QRegExp (Qt5) was removed in Qt6 in favor of QRegularExpression.
// New code should always use QRegularExpression (available since Qt 5.0).
// This macro exists to flag any remaining QRegExp usage.
#ifdef QT_COMMANDER_QT6
#  define QT_COMMANDER_HAS_REGULAR_EXPRESSION  1
#else
   // QRegExp is deprecated in Qt 5.15+, but still available.
#  define QT_COMMANDER_HAS_REGULAR_EXPRESSION  1
#endif

// QTextCodec (Qt5) was removed in Qt6.
// Use QStringConverter / QStringEncoder / QStringDecoder in Qt6.
#ifdef QT_COMMANDER_QT6
#  define QT_COMMANDER_HAS_TEXT_CODEC  0
#else
#  define QT_COMMANDER_HAS_TEXT_CODEC  1
#endif

// ========================== Widgets ==============================

// QWheelEvent::delta() (Qt5) was removed in Qt6;  use angleDelta().
#ifdef QT_COMMANDER_QT6
#  define QT_COMMANDER_WHEEL_DELTA(e)     (e)->angleDelta()
#else
#  define QT_COMMANDER_WHEEL_DELTA(e)     QPoint((e)->delta(), 0)
#endif

// QWheelEvent::orientation() was removed in Qt6.
// Use angleDelta().isNull() or compare via angleDelta() directly.

// QDesktopWidget (Qt5) was removed in Qt6.
// Use QGuiApplication::screens() instead.
#ifdef QT_COMMANDER_QT6
#  define QT_COMMANDER_HAS_DESKTOP_WIDGET  0
#else
#  define QT_COMMANDER_HAS_DESKTOP_WIDGET  1
#endif

// QWidget::frameGeometry() / geometry() are stable across versions.

// QMatrix (Qt5) was removed in Qt6;  always use QTransform.
#ifdef QT_COMMANDER_QT6
#  define QT_COMMANDER_USE_QTRANSFORM  1
#else
#  define QT_COMMANDER_USE_QTRANSFORM  1  // QTransform exists in Qt5 too
#endif

// =========================== OpenGL ==============================

// Qt6 removed QOpenGLFunctions_* includes from public headers.
// Use QOpenGLFunctions (from QtGui) which works on both versions.

// ============================ Quick ==============================

// QQuickItem::stackBefore/stackAfter signature stable.

// QQuickWindow::sendEvent() available since Qt 5.0 (stable).

// ============================ Misc ===============================

// QProcess::splitCommand()  --  available from Qt 5.15 onwards.
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
#  define QT_COMMANDER_HAS_SPLIT_COMMAND  1
#else
#  define QT_COMMANDER_HAS_SPLIT_COMMAND  0
#endif

// qfloat16 availability
#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
#  define QT_COMMANDER_HAS_QFLOAT16  1
#else
#  define QT_COMMANDER_HAS_QFLOAT16  0
#endif

// QRandomGenerator (available since Qt 5.10)
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
#  define QT_COMMANDER_HAS_QRANDOM_GENERATOR  1
#else
#  define QT_COMMANDER_HAS_QRANDOM_GENERATOR  0
#endif

// QTextStream manipulators: Qt5 used global functions (endl, flush, ws, bin,
// oct, hex, dec, uppercasebase, etc.).  Qt6 moved them to the Qt namespace.
// In most code the manipulators "just work" through argument-dependent lookup.
// Notable exception:  Qt::hex / Qt::dec are available in both.

// ======================= Sizing Helpers =========================

// Ensure Q_DECL_OVERRIDE and Q_DECL_FINAL are usable (they are in C++17).

// Qt6 removed the QMutable*Iterator classes for QList/QHash/QMap.
// Use the const-iteration patterns recommended for Qt6.

// =================== End of compat_qt.h =========================
