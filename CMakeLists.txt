cmake_minimum_required(VERSION 3.16)
project(qt-commander LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# The release version is the git tag (vX.Y.Z). This is the only place that
# resolves it for the C++ runtime: a compiled binary cannot read git later, so
# the tag is turned into a compile definition here.
#
#   0.9.0                    tagged
#   0.8.1.dev13              thirteen commits after v0.8.0
#
# The dev shape is exactly what setuptools-scm produces with guess-next-dev and
# no local segment, so the package and the runtime report the same string for
# the same checkout. Neither side encodes a commit hash or a dirty tree: the
# version must be reproducible, and commit identity belongs to the tag.
if(NOT QTC_VERSION)
    set(QTC_VERSION "0.0.0+unknown")
    find_package(Git QUIET)
    if(Git_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --dirty --tags --long
                    --match "v*"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE _qtc_describe
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(_qtc_describe)
            string(REGEX REPLACE "^v" "" _qtc_version "${_qtc_describe}")
            set(_qtc_dirty FALSE)
            if(_qtc_version MATCHES "-dirty$")
                set(_qtc_dirty TRUE)
                string(REGEX REPLACE "-dirty$" "" _qtc_version "${_qtc_version}")
            endif()
            if(_qtc_version MATCHES
               "^([0-9]+\\.[0-9]+)\\.([0-9]+)-([0-9]+)-g([0-9a-f]+)$")
                # v0.8.0-13-g26c06a0 -> 0.8.1.dev13
                math(EXPR _qtc_distance "${CMAKE_MATCH_3}")
                math(EXPR _qtc_patch "${CMAKE_MATCH_2} + 1")
                if(_qtc_distance GREATER 0)
                    set(QTC_VERSION
                        "${CMAKE_MATCH_1}.${_qtc_patch}.dev${_qtc_distance}")
                elseif(_qtc_dirty)
                    # A modified tree is not the release, even on the tag:
                    # setuptools-scm reports X.Y.(Z+1).dev0 for this case.
                    set(QTC_VERSION "${CMAKE_MATCH_1}.${_qtc_patch}.dev0")
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
