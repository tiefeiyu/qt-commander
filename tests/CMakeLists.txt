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
        if(MSVC)
            target_compile_options(${NAME} PRIVATE /utf-8)
            target_link_libraries(${NAME} PRIVATE ws2_32 psapi bcrypt)
        elseif(UNIX)
            target_link_libraries(${NAME} PRIVATE pthread dl)
        endif()
        add_test(NAME ${NAME} COMMAND ${NAME})
    endfunction()

    # --- Unit: DI injector logic (35 tests) ---
    add_injector_test(test_injector_logic
        unit_injector/test_injector_logic.cpp
        ${CMAKE_SOURCE_DIR}/src/injector/injector_di.cpp
    )

    # --- Unit: PE parser vs kernel32.dll (7 tests) ---
    add_injector_test(test_pe_real
        unit_injector/test_pe_real.cpp
    )

    # --- Unit: Win32ProcessOps + CLI args (30 tests) ---
    add_injector_test(test_cli_integration
        unit_injector/test_cli_integration.cpp
        ${CMAKE_SOURCE_DIR}/src/injector/injector_di.cpp
    )

    # --- Unit: injector_win.cpp helpers (15 tests) ---
    add_injector_test(test_win32_coverage
        unit_injector/test_win32_coverage.cpp
    )

    # --- Unit: Socket utils (12 tests) ---
    # socket_utils.cpp is #include'd by the test file, not linked separately
    add_injector_test(test_socket_utils
        unit_injector/test_socket_utils.cpp
    )

    # --- Unit: LoadLibraryW failure paths (2 tests) ---
    add_injector_test(test_injector_errors
        unit_injector/test_injector_errors.cpp
    )

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

    # --- Unit: PE parser synthetic (10 tests) ---
    add_injector_test(test_pe_parser
        unit_injector/test_pe_parser.cpp
    )

    # --- E2E: Full lifecycle (12 tests) ---
    # Depends on: qt-injector, libqt-commander, qt-widget-test
    add_injector_test(test_e2e
        unit_injector/test_e2e.cpp
    )
    set_tests_properties(test_e2e PROPERTIES
        DEPENDS "qt-injector;qt-widget-test"
        ENVIRONMENT "PATH=${CMAKE_BINARY_DIR}\;$ENV{PATH}"
    )

    # --- E2E: Exit codes 3 + 6 (2 tests) ---
    add_injector_test(test_e2e_exit_codes
        unit_injector/test_e2e_exit_codes.cpp
    )
    set_tests_properties(test_e2e_exit_codes PROPERTIES
        DEPENDS "qt-injector;qt-widget-test"
        ENVIRONMENT "PATH=${CMAKE_BINARY_DIR}\;$ENV{PATH}"
    )

    # --- E2E: Exit codes 4 + 5 (2 tests) ---
    add_injector_test(test_exit45
        unit_injector/test_exit45.cpp
    )
    set_tests_properties(test_exit45 PROPERTIES
        DEPENDS "qt-injector;qt-widget-test"
        ENVIRONMENT "PATH=${CMAKE_BINARY_DIR}\;$ENV{PATH}"
    )
endif()

# --- Library component unit tests ---
if(BUILD_LIBRARY)
    add_subdirectory(unit_library)
endif()
