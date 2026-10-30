cmake_minimum_required(VERSION 3.20)
project(qt-matrix-test LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
find_package(Qt${QT_MAJOR_VERSION} REQUIRED COMPONENTS
    Core Gui Widgets Quick QuickWidgets Qml
)

add_subdirectory(../../../src qt-commander)

add_executable(qt-matrix-test main.cpp)
target_include_directories(qt-matrix-test PRIVATE
    ../../../src/include
)
target_link_libraries(qt-matrix-test PRIVATE
    qt_commander
    Qt${QT_MAJOR_VERSION}::Core
    Qt${QT_MAJOR_VERSION}::Gui
    Qt${QT_MAJOR_VERSION}::Widgets
    Qt${QT_MAJOR_VERSION}::Quick
    Qt${QT_MAJOR_VERSION}::QuickWidgets
    Qt${QT_MAJOR_VERSION}::Qml
)

configure_file(main.qml ${CMAKE_CURRENT_BINARY_DIR}/main.qml COPYONLY)
