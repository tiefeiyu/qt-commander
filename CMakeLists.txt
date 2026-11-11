cmake_minimum_required(VERSION 3.16)
project(qt-commander LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# The release version is the git tag (vX.Y.Z). This is the only place that
# resolves it for the C++ runtime: a compiled binary cannot read git later, so
# the tag is turned into a compile definition here.
#
#   0.9.0                             tagged, clean
#   0.8.1.dev11+g03c552a7b.d20261111   eleven commits after v0.8.0
#
# The dev shape mirrors setuptools-scm (guess-next-dev plus node-and-date) so
# the package and the runtime report the same string for the same checkout.
# Neither side marks a dirty working tree: a release is built from a tag.
if(NOT QTC_VERSION)
    set(QTC_VERSION "0.0.0+unknown")
    find_package(Git QUIET)
    if(Git_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --dirty --tags --long
                    --abbrev=7 --match "v*"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE _qtc_describe
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(_qtc_describe)
            string(REGEX REPLACE "^v" "" _qtc_version "${_qtc_describe}")
            # Neither side marks a dirty tree: a release is built from a tag,
            # and setuptools-scm's node-and-date scheme does not encode it
            # either, so drop it to keep the two strings identical.
            string(REGEX REPLACE "-dirty$" "" _qtc_version "${_qtc_version}")
            if(_qtc_version MATCHES
               "^([0-9]+\\.[0-9]+)\\.([0-9]+)-([0-9]+)-g([0-9a-f]+)$")
                math(EXPR _qtc_distance "${CMAKE_MATCH_3}")
                if(_qtc_distance GREATER 0)
                    # v0.8.0-11-g03c552a -> 0.8.1.dev11+g03c552a.d20261111
                    math(EXPR _qtc_patch "${CMAKE_MATCH_2} + 1")
                    set(QTC_VERSION
                        "${CMAKE_MATCH_1}.${_qtc_patch}.dev${_qtc_distance}+g${CMAKE_MATCH_4}")
                    execute_process(
                        COMMAND "${GIT_EXECUTABLE}" log -1 --format=%cd
                                --date=format:%Y%m%d
                        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                        OUTPUT_VARIABLE _qtc_commit_date
                        OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_QUIET
                    )
                    # CMake's regex engine has no {n} intervals, so spell out 8.
                    if(_qtc_commit_date MATCHES
                       "^[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]$")
                        string(APPEND QTC_VERSION ".d${_qtc_commit_date}")
                    endif()
                else()
                    # HEAD is the tag: --long still appends -0-g<hash>.
                    set(QTC_VERSION "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}")
                endif()
            else()
                set(QTC_VERSION "${_qtc_version}")
            endif()
        endif()
    endif()
endif()
set(QTC_VERSION "${QTC_VERSION}" CACHE STRING "Release version, from the git tag")
message(STATUS "qt-commander version: ${QTC_VERSION}")

set(QT_MAJOR_VERSION "5" CACHE STRING "Qt major version (5 or 6)")
find_package(Qt${QT_MAJOR_VERSION} REQUIRED COMPONENTS Core Gui Widgets Network)

add_subdirectory(src)
