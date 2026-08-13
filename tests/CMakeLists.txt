# Qt bin dir (tests that spawn Qt processes or link Qt DLLs need it on PATH
# -- ctest does not inherit the developer shell's PATH).
if(BUILD_LIBRARY AND DEFINED Qt${QT_MAJOR_VERSION}_DIR)
    get_filename_component(QT_COMMANDER_QT_BIN
        "${Qt${QT_MAJOR_VERSION}_DIR}" DIRECTORY)   # .../lib/cmake/Qt5
    get_filename_component(QT_COMMANDER_QT_BIN
        "${QT_COMMANDER_QT_BIN}" DIRECTORY)         # .../lib
    get_filename_component(QT_COMMANDER_QT_BIN
        "${QT_COMMANDER_QT_BIN}" DIRECTORY)         # .../<Qt root>
    set(QT_COMMANDER_QT_BIN "${QT_COMMANDER_QT_BIN}/bin")
endif()

# --- Injector unit tests ---
if(BUILD_INJECTOR)
    # Common settings for injector tests
    set(INJECTOR_INCLUDES
        ${CMAKE_SOURCE_DIR}/src/injector
        ${CMAKE_SOURCE_DIR}/src/common
        ${CMAKE_SOURCE_DIR}/src/library
    )
    function(add_injector_test NAME SOURCES)
        add_executable(${NAME} ${SOURCES} ${ARGN})
        target_include_directories(${NAME} PRIVATE ${INJECTOR_INCLUDES})
        target_compile_features(${NAME} PRIVATE cxx_std_17)
        if(WIN32)
            target_link_libraries(${NAME} PRIVATE ws2_32 psapi bcrypt)
            if(MSVC)
                target_compile_options(${NAME} PRIVATE /utf-8)
            endif()
        elseif(UNIX)
            target_link_libraries(${NAME} PRIVATE pthread dl)
            # Tests that link injector_di.cpp need the ptrace/elf sources
            # on Linux (PosixProcessOps depends on them).
            foreach(_s ${SOURCES} ${ARGN})
                if(_s MATCHES "injector_di\\.cpp")
                    target_sources(${NAME} PRIVATE
                        ${CMAKE_SOURCE_DIR}/src/injector/ptrace_ops.cpp
                        ${CMAKE_SOURCE_DIR}/src/injector/elf_parser.cpp
                    )
                    break()
                endif()
            endforeach()
        endif()
        add_test(NAME ${NAME} COMMAND ${NAME})
        # Every test gets a timeout so a hang fails instead of blocking
        # the whole ctest run forever.  E2E suites (which spawn injector
        # + test apps) are given a longer budget by their callers.
        set_tests_properties(${NAME} PROPERTIES TIMEOUT 120)
        # The E2E suites spawn qt-injector / the test apps; put their
        # output directories (and the Qt bin dir, for the apps' Qt DLLs)
        # on PATH so they resolve from any build tree layout.
        set(_qtc_bin_dirs
            "${CMAKE_BINARY_DIR}/src/injector"
            "${CMAKE_BINARY_DIR}/src/library"
            "${CMAKE_BINARY_DIR}/tests/test-apps/widget"
            "${CMAKE_BINARY_DIR}/tests/test-apps/qml"
            "${QT_COMMANDER_QT_BIN}")
        string(JOIN ";" _qtc_path ${_qtc_bin_dirs})
        if(WIN32)
            # Semicolons inside the PATH value must be escaped, otherwise CMake
            # splits the ENVIRONMENT property into multiple list entries and
            # ctest applies only the first one.
            set(_qtc_env "PATH=${_qtc_path}\;$ENV{PATH}")
            string(REPLACE ";" "\\;" _qtc_env "${_qtc_env}")
            set_tests_properties(${NAME} PROPERTIES
                ENVIRONMENT "${_qtc_env}")
        else()
            # Linux: use colon-separated PATH and LD_LIBRARY_PATH.
            string(JOIN ":" _qtc_path_colon ${_qtc_bin_dirs})
            set_tests_properties(${NAME} PROPERTIES
                ENVIRONMENT "PATH=${_qtc_path_colon}:$ENV{PATH};LD_LIBRARY_PATH=${_qtc_path_colon}:$ENV{LD_LIBRARY_PATH}")
        endif()
    endfunction()

    # --- Unit: DI injector logic (35 tests) ---
    add_injector_test(test_injector_logic
        unit_injector/test_injector_logic.cpp
        ${CMAKE_SOURCE_DIR}/src/injector/injector_di.cpp
    )

    # --- Unit: PE parser vs kernel32.dll (7 tests, Windows only) ---
    if(WIN32)
        add_injector_test(test_pe_real
            unit_injector/test_pe_real.cpp
        )
    endif()

    # --- Unit: Win32ProcessOps + CLI args (30 tests, Windows only) ---
    if(WIN32)
        add_injector_test(test_cli_integration
            unit_injector/test_cli_integration.cpp
            ${CMAKE_SOURCE_DIR}/src/injector/injector_di.cpp
        )
    endif()

    # --- Unit: injector_win.cpp helpers (15 tests, Windows only) ---
    if(WIN32)
        add_injector_test(test_win32_coverage
            unit_injector/test_win32_coverage.cpp
        )
    endif()

    # --- Unit: Socket utils (12 tests, cross-platform via #ifdef) ---
    add_injector_test(test_socket_utils
        unit_injector/test_socket_utils.cpp
    )

    # --- Unit: LoadLibraryW failure paths (2 tests, Windows only) ---
    if(WIN32)
        add_injector_test(test_injector_errors
            unit_injector/test_injector_errors.cpp
        )
    endif()

    # --- Unit: Framing cross-validation (5 tests) ---
    add_injector_test(test_framing_cross
        unit_injector/test_framing_cross.cpp
    )

    # --- Unit: CLI argument validation (5 tests) ---
    add_injector_test(test_injector_cli
        unit_injector/test_injector_cli.cpp
    )

    # --- Unit: Port poll algorithm (8 tests) ---
    add_injector_test(test_port_poll
        unit_injector/test_port_poll.cpp
    )

    # --- Unit: PE parser synthetic (10 tests, cross-platform) ---
    add_injector_test(test_pe_parser
        unit_injector/test_pe_parser.cpp
    )

    # --- Unit: PE import parser + dependency closure (16 tests, Windows only) ---
    if(WIN32)
        add_injector_test(test_import_parser
            unit_injector/test_import_parser.cpp
        )
    endif()

    # --- E2E: Full lifecycle (12 tests, Windows only for now) ---
    # Depends on: qt-injector, libqt-commander, qt-widget-test
    if(WIN32)
        add_injector_test(test_e2e
            unit_injector/test_e2e.cpp
        )
        set_tests_properties(test_e2e PROPERTIES
            DEPENDS "qt-injector;qt-widget-test"
            TIMEOUT 300
        )
    endif()

    # --- E2E: Exit codes 3 + 6 (2 tests, Windows only) ---
    if(WIN32)
        add_injector_test(test_e2e_exit_codes
            unit_injector/test_e2e_exit_codes.cpp
        )
        set_tests_properties(test_e2e_exit_codes PROPERTIES
            DEPENDS "qt-injector;qt-widget-test"
            TIMEOUT 300
        )
    endif()

    # --- E2E: Exit codes 4 + 5 (2 tests, Windows only) ---
    if(WIN32)
        add_injector_test(test_exit45
            unit_injector/test_exit45.cpp
        )
        set_tests_properties(test_exit45 PROPERTIES
            DEPENDS "qt-injector;qt-widget-test"
            TIMEOUT 300
        )
    endif()

    # --- Full-stack integration: launch app -> inject -> RPC round-trips ---
    # (Windows only: uses CreateRemoteThread injection + psapi directly;
    # WITH_QML needed for the qt-qml-test target)
    if(WIN32 AND WITH_QML)
        add_injector_test(test_integration
            integration/test_integration.cpp
            ${CMAKE_SOURCE_DIR}/src/common/socket_utils.cpp
        )
        # The file includes "common/socket_utils.h" (src-root relative).
        target_include_directories(test_integration PRIVATE
            ${CMAKE_SOURCE_DIR}/src)
        target_compile_definitions(test_integration PRIVATE
            QT_WIDGET_TEST_APP="$<TARGET_FILE:qt-widget-test>"
            QT_QML_TEST_APP="$<TARGET_FILE:qt-qml-test>"
            QT_COMMANDER_DLL="$<TARGET_FILE:qt-commander-lib>")
        add_dependencies(test_integration
            qt-widget-test qt-qml-test qt-commander-lib)
        set_tests_properties(test_integration PROPERTIES
            TIMEOUT 900)
    endif()
endif()

# --- Python unit tests + E2E preload verification ---------------------------
# Registered here so `ctest` runs EVERYTHING (C++ suites, pytest, and the
# E2E preload scenarios) from one build tree.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME python_unit_tests
        COMMAND ${Python3_EXECUTABLE} -m pytest
                ${CMAKE_SOURCE_DIR}/tests -q)
    set_tests_properties(python_unit_tests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        LABELS "python"
        TIMEOUT 300)

    # Deployment-level E2E: needs the qt_build artifacts in .qt-commander/bin
    # (injector + library + Qt dependency closure DLLs).
    add_test(NAME verify_preload
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/verify_preload.py)
    set_tests_properties(verify_preload PROPERTIES
        LABELS "e2e"
        TIMEOUT 900
        ENVIRONMENT "QT_BIN=${QT_COMMANDER_QT_BIN};QT_QML_APP=${CMAKE_BINARY_DIR}/tests/test-apps/qml/qt-qml-test.exe;QT_WIDGET_APP=${CMAKE_BINARY_DIR}/tests/test-apps/widget/qt-widget-test.exe")
endif()

# --- Library component unit tests ---
if(BUILD_LIBRARY)
    add_subdirectory(unit_library)
endif()
