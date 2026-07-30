# --- Injector unit tests ---
if(BUILD_INJECTOR)
    # Framing cross-validation test
    add_executable(test_framing_cross unit_injector/test_framing_cross.cpp)
    target_include_directories(test_framing_cross PRIVATE ${CMAKE_SOURCE_DIR}/src/common)
    add_test(NAME test_framing_cross COMMAND test_framing_cross)

    # Injector CLI test
    add_executable(test_injector_cli unit_injector/test_injector_cli.cpp
        ${CMAKE_SOURCE_DIR}/src/injector/main.cpp
        ${CMAKE_SOURCE_DIR}/src/injector/injector_win.cpp
        ${CMAKE_SOURCE_DIR}/src/common/socket_utils.cpp
    )
    target_include_directories(test_injector_cli PRIVATE
        ${CMAKE_SOURCE_DIR}/src/common
        ${CMAKE_SOURCE_DIR}/src/library
        ${CMAKE_SOURCE_DIR}/src/injector
    )
    target_compile_definitions(test_injector_cli PRIVATE BUILD_TESTS)
    if(MSVC)
        target_link_libraries(test_injector_cli PRIVATE psapi bcrypt ws2_32)
    endif()
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
