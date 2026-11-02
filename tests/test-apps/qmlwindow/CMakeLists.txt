cmake_minimum_required(VERSION 3.16)
project(qt-qmlwindow-test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

set(QT_MAJOR_VERSION "5" CACHE STRING "Qt major version (5 or 6)")
find_package(Qt${QT_MAJOR_VERSION} REQUIRED COMPONENTS Quick Qml QuickWidgets Network)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../.. qt-commander)

add_executable(qmlwindow-test main.cpp)
target_link_libraries(qmlwindow-test PRIVATE
    qt_commander
    Qt${QT_MAJOR_VERSION}::Quick
    Qt${QT_MAJOR_VERSION}::Qml
)
target_compile_definitions(qmlwindow-test PRIVATE
    QML_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/main.qml"
)
if(MSVC)
    target_compile_options(qmlwindow-test PRIVATE /utf-8)
endif()
