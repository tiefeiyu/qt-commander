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
