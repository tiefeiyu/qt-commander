cmake_minimum_required(VERSION 3.20)
project(qt-injected-test LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
find_package(Qt${QT_MAJOR_VERSION} REQUIRED COMPONENTS Core Gui Widgets)

add_executable(qt-injected-test main.cpp)
target_link_libraries(qt-injected-test PRIVATE
    Qt${QT_MAJOR_VERSION}::Core
    Qt${QT_MAJOR_VERSION}::Gui
    Qt${QT_MAJOR_VERSION}::Widgets
)
