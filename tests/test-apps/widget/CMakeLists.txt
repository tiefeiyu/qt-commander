set(CMAKE_AUTOMOC ON)

set(APP_SOURCES
    main.cpp
    mainwindow.cpp
    mainwindow.h
    test_dialog.cpp
    test_dialog.h
)

add_executable(qt-widget-test ${APP_SOURCES})
target_include_directories(qt-widget-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(QT_MAJOR_VERSION EQUAL 6)
    target_link_libraries(qt-widget-test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Widgets
    )
else()
    target_link_libraries(qt-widget-test PRIVATE
        Qt5::Core Qt5::Gui Qt5::Widgets
    )
endif()

set_target_properties(qt-widget-test PROPERTIES
    WIN32_EXECUTABLE TRUE
)

# Component-scoped so a QML-only build tree can install just its own app:
#   cmake --install <build> --component widget-tests
install(TARGETS qt-widget-test RUNTIME DESTINATION bin COMPONENT widget-tests)

# Deploy the Qt runtime DLLs next to the exe so the app runs standalone
# right after `cmake --install`. Platform deploy tools all share the same
# CLI shape (windeployqt / macdeployqt / linuxdeployqt); the per-platform
# integration is reworked when multi-platform deploy is added.
if(WIN32)
    if(TARGET Qt${QT_MAJOR_VERSION}::qmake)
        get_target_property(QT_QMAKE Qt${QT_MAJOR_VERSION}::qmake IMPORTED_LOCATION)
    endif()
    if(NOT QT_QMAKE)
        set(QT_QMAKE "${QT_QMAKE_EXECUTABLE}")
    endif()
    if(QT_QMAKE)
        get_filename_component(QT_BIN_DIR "${QT_QMAKE}" DIRECTORY)
        set(WINDEPLOYQT_EXE "${QT_BIN_DIR}/windeployqt.exe")
    endif()
    if(WINDEPLOYQT_EXE AND EXISTS "${WINDEPLOYQT_EXE}")
        install(CODE "
            execute_process(COMMAND \"${WINDEPLOYQT_EXE}\"
                --release --no-translations --no-compiler-runtime
                \"\$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/qt-widget-test.exe\"
                RESULT_VARIABLE _wdq_result)
            if(NOT _wdq_result EQUAL 0)
                message(WARNING \"windeployqt failed (exit \${_wdq_result})\")
            endif()
        " COMPONENT widget-tests)
    else()
        message(WARNING
            "windeployqt not found; Qt DLLs must be deployed manually")
    endif()
endif()
