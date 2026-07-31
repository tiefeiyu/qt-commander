# --- Injector unit tests ---
if(BUILD_INJECTOR)
    # Framing cross-validation test
    add_executable(test_framing_cross unit_injector/test_framing_cross.cpp)
    target_include_directories(test_framing_cross PRIVATE ${CMAKE_SOURCE_DIR}/src/common)
    if(MSVC)
        target_link_libraries(test_framing_cross PRIVATE ws2_32)
    endif()
    add_test(NAME test_framing_cross COMMAND test_framing_cross)

    # Injector CLI test (no main.cpp — only tests argument validation logic)
    add_executable(test_injector_cli unit_injector/test_injector_cli.cpp)
    add_test(NAME test_injector_cli COMMAND test_injector_cli)

    # Port poll algorithm test
    add_executable(test_port_poll unit_injector/test_port_poll.cpp)
    add_test(NAME test_port_poll COMMAND test_port_poll)

    # PE parser test
    add_executable(test_pe_parser unit_injector/test_pe_parser.cpp)
    add_test(NAME test_pe_parser COMMAND test_pe_parser)
endif()

# --- Library component unit tests ---
if(BUILD_LIBRARY)
    add_subdirectory(unit_library)
endif()
