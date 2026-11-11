# Version and commit resolution lives here, not in the top-level CMakeLists:
# the linked test apps (and anyone vendoring the runtime) add this directory to
# their own project, and they must get a real version too.

# The release version is the git tag (vX.Y.Z). A compiled binary cannot read git
# later, so the tag is resolved once here and compiled in.
#
#   0.9.0                    tagged
#   0.8.1.dev13              thirteen commits after v0.8.0
#
# The dev shape is exactly what setuptools-scm produces with guess-next-dev and
# no local segment, so the package and the runtime report the same string for
# the same checkout. Neither side encodes a commit hash or a dirty tree: the
# version must be reproducible, and commit identity belongs to the tag.
set(_qtc_repo_root "${CMAKE_CURRENT_SOURCE_DIR}/..")
find_package(Git QUIET)
if(NOT QTC_VERSION)
    set(QTC_VERSION "0.0.0+unknown")
    if(Git_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --dirty --tags --long
                    --match "v*"
            WORKING_DIRECTORY "${_qtc_repo_root}"
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

# Commit identity is not part of the version (a version must be reproducible),
# but an artifact should know exactly which checkout produced it. This is
# written next to the build and copied into manifest.json by the packaging
# tools, so `qt-commander doctor` can print it without a git checkout.
set(QTC_COMMIT "" CACHE STRING "Commit the build came from")
set(QTC_DESCRIBE "" CACHE STRING "git describe for the build")
if(Git_FOUND)
    if(NOT QTC_COMMIT)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
            WORKING_DIRECTORY "${_qtc_repo_root}"
            OUTPUT_VARIABLE _qtc_commit
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(_qtc_commit)
            set(QTC_COMMIT "${_qtc_commit}")
        endif()
    endif()
    if(NOT QTC_DESCRIBE)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --tags --long --dirty
                    --always --match "v*"
            WORKING_DIRECTORY "${_qtc_repo_root}"
            OUTPUT_VARIABLE _qtc_describe_full
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(_qtc_describe_full)
            set(QTC_DESCRIBE "${_qtc_describe_full}")
        endif()
    endif()
endif()
file(WRITE "${CMAKE_BINARY_DIR}/qtc-build-info.json"
     "{\n  \"version\": \"${QTC_VERSION}\",\n  \"commit\": \"${QTC_COMMIT}\",\n  \"describe\": \"${QTC_DESCRIBE}\"\n}\n")

message(STATUS "qt-commander version: ${QTC_VERSION}")
if(QTC_COMMIT)
    message(STATUS "qt-commander commit: ${QTC_DESCRIBE} (${QTC_COMMIT})")
endif()

find_package(Qt${QT_MAJOR_VERSION} REQUIRED COMPONENTS
    Core Gui Widgets Network Quick QuickWidgets
)

add_library(qt_commander STATIC
    library/agent.cpp
    library/protocol.cpp
    library/server.cpp
)

set_target_properties(qt_commander PROPERTIES
    AUTOMOC ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_include_directories(qt_commander
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set(QTC_BUILD_ID
    "qt${QT_MAJOR_VERSION}/${CMAKE_CXX_COMPILER_ID}-${CMAKE_SYSTEM_PROCESSOR}/${CMAKE_BUILD_TYPE}"
)
target_compile_definitions(qt_commander PRIVATE
    QT_COMMANDER_BUILD_ID="${QTC_BUILD_ID}"
)
# Never define an empty version: version.h's fallback only applies when the
# macro is absent, and a runtime reporting "" is worse than "unknown".
if(QTC_VERSION)
    target_compile_definitions(qt_commander PRIVATE
        QT_COMMANDER_VERSION="${QTC_VERSION}"
    )
endif()
if(MSVC)
    target_compile_options(qt_commander PRIVATE /utf-8)
endif()

target_link_libraries(qt_commander
    PUBLIC Qt${QT_MAJOR_VERSION}::Core Qt${QT_MAJOR_VERSION}::Gui Qt${QT_MAJOR_VERSION}::Widgets Qt${QT_MAJOR_VERSION}::Network Qt${QT_MAJOR_VERSION}::Quick Qt${QT_MAJOR_VERSION}::QuickWidgets
)

# The injection library is built on every platform that can load one. On macOS
# it is loaded at launch through DYLD_INSERT_LIBRARIES (there is no attach-to-a-
# running-process injector, by OS design); see docs/design.md §8.
if(WIN32)
    add_library(qt_commander_inject SHARED library/inject_main.cpp)
    target_link_libraries(qt_commander_inject PRIVATE qt_commander)
    set_target_properties(qt_commander_inject PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        PREFIX ""
    )
    if(MINGW)
        target_link_options(qt_commander_inject PRIVATE
            -static-libgcc -static-libstdc++
        )
    endif()
elseif(UNIX)
    add_library(qt_commander_inject SHARED library/inject_main_unix.cpp)
    target_link_libraries(qt_commander_inject PRIVATE qt_commander)
    set_target_properties(qt_commander_inject PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
endif()

# The native injector executable is only meaningful where a process can be
# injected into from outside: Windows and Linux.
if(WIN32 OR (UNIX AND NOT APPLE))
    add_subdirectory(injector)
endif()
