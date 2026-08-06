cmake_minimum_required(VERSION 3.16)
project(qt-commander LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Threads are used by server (std::thread) and required on Linux/macOS
find_package(Threads REQUIRED)

# MSVC: treat all source files as UTF-8 (suppresses C4819 warning)
if(MSVC)
    add_compile_options(/utf-8)
endif()

# MinGW: the compiler runtime (libgcc_s_seh-1.dll / libstdc++-6.dll /
# libwinpthread-1.dll) must match the compiler used to BUILD, not the Qt
# kit's bundled (possibly older) runtime.  Drop the compiler's runtime next
# to the injector output dir, which is first on every test's PATH; the Qt
# kit's runtime may lack symbols the newer compiler emits.
if(MINGW)
    get_filename_component(_qtc_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(_rt libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
        if(EXISTS "${_qtc_compiler_bin}/${_rt}")
            file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/src/injector")
            configure_file(
                "${_qtc_compiler_bin}/${_rt}"
                "${CMAKE_BINARY_DIR}/src/injector/${_rt}" COPYONLY)
        endif()
    endforeach()
endif()

# --- Options ---
option(BUILD_INJECTOR   "Build qt-injector standalone CLI"  ON)
option(BUILD_INJECTOR   "Build qt-injector standalone CLI"  ON)
option(BUILD_LIBRARY  "Build injection library"        OFF)
option(BUILD_TESTS    "Build tests"                    ON)
option(WITH_QML       "Build with QML/QQuick support"  ON)
set(QT_MAJOR_VERSION "5" CACHE STRING "Qt major version (5 or 6)")

# When tests are on, also build the library and test apps (if Qt available)
if(BUILD_TESTS)
    set(BUILD_LIBRARY ON)
endif()

# --- Qt detection (graceful: if not found, Qt targets are skipped) ---
set(_need_qt OFF)
if(BUILD_LIBRARY)
    set(_need_qt ON)
endif()

if(_need_qt)
    if(QT_MAJOR_VERSION EQUAL 6)
        set(_qt_comps Core Gui Widgets)
        if(WITH_QML)
            list(APPEND _qt_comps Quick Qml QuickWidgets)
        endif()
        find_package(Qt6 QUIET COMPONENTS ${_qt_comps})
        if(NOT Qt6_FOUND)
            message(STATUS "Qt6 not found -- library and test apps disabled")
            set(BUILD_LIBRARY OFF)
        endif()
    else()
        set(_qt_comps Core Gui Widgets)
        if(WITH_QML)
            list(APPEND _qt_comps Quick QuickWidgets)
        endif()
        find_package(Qt5 QUIET COMPONENTS ${_qt_comps})
        if(NOT Qt5_FOUND)
            message(STATUS "Qt5 not found -- library and test apps disabled")
            set(BUILD_LIBRARY OFF)
        endif()
    endif()
endif()

# --- Subdirectories ---
if(BUILD_INJECTOR)
    add_subdirectory(src/injector)
endif()

if(BUILD_LIBRARY)
    add_subdirectory(src/library)
endif()

if(BUILD_TESTS)
    enable_testing()
    # Test apps are always built alongside tests (if Qt is available and library is enabled)
    if(BUILD_LIBRARY)
        # Always built: pure QWidget test app (no QML dependency)
        add_subdirectory(tests/test-apps/widget)
        if(WITH_QML)
            add_subdirectory(tests/test-apps/qml)
        endif()
        # Occlusion scenarios: overlapping layers for prune end-to-end
        # verification (widget app has no QML dependency)
        add_subdirectory(tests/test-apps/occlusion)
    endif()
    add_subdirectory(tests)
endif()
