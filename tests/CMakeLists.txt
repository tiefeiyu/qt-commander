# --- Framing test (11 tests) ---
add_executable(test_framing unit_server/test_framing.cpp ${CMAKE_SOURCE_DIR}/src/common/framing.cpp)
target_include_directories(test_framing PRIVATE ${CMAKE_SOURCE_DIR}/src/common)
add_test(NAME test_framing COMMAND test_framing)

# --- Socket utils test (7 tests) ---
add_executable(test_socket_utils unit_server/test_socket_utils.cpp ${CMAKE_SOURCE_DIR}/src/common/socket_utils.cpp)
target_include_directories(test_socket_utils PRIVATE ${CMAKE_SOURCE_DIR}/src/common ${CMAKE_SOURCE_DIR}/src)
if(WIN32)
    target_link_libraries(test_socket_utils PRIVATE ws2_32)
endif()
add_test(NAME test_socket_utils COMMAND test_socket_utils)

# --- JSON-RPC test (10 tests) ---
add_executable(test_json_rpc unit_server/test_json_rpc.cpp)
add_test(NAME test_json_rpc COMMAND test_json_rpc)

# --- Protocol test (10 tests) ---
add_executable(test_protocol unit_server/test_protocol.cpp ${CMAKE_SOURCE_DIR}/src/server/mcp/protocol.cpp)
target_include_directories(test_protocol PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external
)
add_test(NAME test_protocol COMMAND test_protocol)

# --- Session manager test (8 tests) ---
add_executable(test_session_manager unit_server/test_session_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/server/session/session_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/server/session/session.cpp
    ${CMAKE_SOURCE_DIR}/src/common/socket_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/common/framing.cpp
)
target_include_directories(test_session_manager PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external
)
if(WIN32)
    target_link_libraries(test_session_manager PRIVATE ws2_32)
endif()
add_test(NAME test_session_manager COMMAND test_session_manager)

# --- Build manager test (3 tests) ---
add_executable(test_build_manager unit_server/test_build_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/server/build/build_manager.cpp
)
target_include_directories(test_build_manager PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external
)
if(WIN32)
    target_link_libraries(test_build_manager PRIVATE ws2_32)
endif()
add_test(NAME test_build_manager COMMAND test_build_manager)

# --- Integration test ---
# Integration test — always built when library is available.
# Uses qt-widget-test (always) plus qt-test-app / qt-qml-test (WITH_QML only).
if(BUILD_LIBRARY)
    add_executable(test_integration integration/test_integration.cpp
        ${CMAKE_SOURCE_DIR}/src/common/socket_utils.cpp
        ${CMAKE_SOURCE_DIR}/src/common/framing.cpp)
    target_include_directories(test_integration PRIVATE
        ${CMAKE_SOURCE_DIR}/src/common
        ${CMAKE_SOURCE_DIR}/src
    )
    set(_integ_defs QT_WIDGET_TEST_APP="$<TARGET_FILE:qt-widget-test>"
                    QT_COMMANDER_DLL="$<TARGET_FILE:qt-commander-lib>")
    if(WITH_QML)
        list(APPEND _integ_defs QT_QML_TEST_APP="$<TARGET_FILE:qt-qml-test>")
    else()
        list(APPEND _integ_defs QT_QML_TEST_APP="")
    endif()
    target_compile_definitions(test_integration PRIVATE ${_integ_defs})
    if(WIN32)
        target_link_libraries(test_integration PRIVATE ws2_32 psapi)
    endif()
    add_test(NAME test_integration COMMAND test_integration)
    set_tests_properties(test_integration PROPERTIES TIMEOUT 60)
endif()

# Quick inject tool (for manual use)
add_executable(quick_inject ../tools/quick_inject.cpp
    ${CMAKE_SOURCE_DIR}/src/common/socket_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/common/framing.cpp)
target_include_directories(quick_inject PRIVATE ${CMAKE_SOURCE_DIR}/src/common ${CMAKE_SOURCE_DIR}/src)
if(WIN32)
    target_link_libraries(quick_inject PRIVATE ws2_32 psapi)
endif()

# --- Library component unit tests ---
add_subdirectory(unit_library)
