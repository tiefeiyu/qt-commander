cmake_minimum_required(VERSION 3.16)
project(qt-large-test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

set(QT_MAJOR_VERSION "5" CACHE STRING "Qt major version (5 or 6)")
find_package(Qt${QT_MAJOR_VERSION} REQUIRED COMPONENTS Widgets Network)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../src qt-commander)

add_executable(qt-large-test main.cpp)
target_link_libraries(qt-large-test PRIVATE qt_commander)

if(MSVC)
    target_compile_options(qt-large-test PRIVATE /utf-8)
endif()
