cmake_minimum_required(VERSION 3.20)
project(qt-modal-test LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
find_package(Qt${QT_MAJOR_VERSION} REQUIRED COMPONENTS Core Gui Widgets)

add_subdirectory(../../../src qt-commander)

add_executable(qt-modal-test main.cpp)
target_include_directories(qt-modal-test PRIVATE
    ../../../src/include
)
target_link_libraries(qt-modal-test PRIVATE
    qt_commander
    Qt${QT_MAJOR_VERSION}::Core
    Qt${QT_MAJOR_VERSION}::Gui
    Qt${QT_MAJOR_VERSION}::Widgets
)
