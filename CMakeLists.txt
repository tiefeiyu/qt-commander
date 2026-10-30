cmake_minimum_required(VERSION 3.16)
project(qt-commander LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

set(QT_MAJOR_VERSION "5" CACHE STRING "Qt major version (5 or 6)")
find_package(Qt${QT_MAJOR_VERSION} REQUIRED COMPONENTS Core Gui Widgets Network)

add_subdirectory(src)
