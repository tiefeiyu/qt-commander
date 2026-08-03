if(NOT QT_MAJOR_VERSION)
    set(QT_MAJOR_VERSION "5")
endif()
find_package(Qt${QT_MAJOR_VERSION} QUIET COMPONENTS Core Gui Widgets)
if(NOT Qt${QT_MAJOR_VERSION}_FOUND)
    message(STATUS "Qt not found — skipping library unit tests")
    return()
endif()

# Derive Qt bin path from the found Qt installation.
# Qt<version>_DIR points to <prefix>/lib/cmake/Qt<version>
get_filename_component(_qt_prefix "${Qt${QT_MAJOR_VERSION}_DIR}" DIRECTORY)
get_filename_component(_qt_prefix "${_qt_prefix}" DIRECTORY)
get_filename_component(_qt_prefix "${_qt_prefix}" DIRECTORY)
set(QT_BIN_PATH "${_qt_prefix}/bin")
message(STATUS "Qt bin path: ${QT_BIN_PATH}")

# --- ElementMap test (12 tests) ---
add_executable(test_element_map
    test_element_map.cpp
    ${CMAKE_SOURCE_DIR}/src/library/core/element_map.cpp
)
target_include_directories(test_element_map PRIVATE ${CMAKE_SOURCE_DIR}/src/library/core)
target_link_libraries(test_element_map PRIVATE Qt${QT_MAJOR_VERSION}::Core)
set_target_properties(test_element_map PROPERTIES AUTOMOC ON)
add_test(NAME test_element_map COMMAND test_element_map)

# --- Selector test (12 tests) ---
add_executable(test_selector
    test_selector.cpp
    ${CMAKE_SOURCE_DIR}/src/library/selector/selector.cpp
)
if(WITH_QML)
    target_sources(test_selector PRIVATE
        ${CMAKE_SOURCE_DIR}/src/library/core/event_injector.cpp)
endif()
target_include_directories(test_selector PRIVATE ${CMAKE_SOURCE_DIR}/src/library
    # QtGui private/QPA headers (QWindowSystemInterface for real click tests)
    ${Qt${QT_MAJOR_VERSION}Gui_PRIVATE_INCLUDE_DIRS}
)
target_link_libraries(test_selector PRIVATE Qt${QT_MAJOR_VERSION}::Core Qt${QT_MAJOR_VERSION}::Gui Qt${QT_MAJOR_VERSION}::Widgets)
if(WITH_QML)
    target_link_libraries(test_selector PRIVATE Qt${QT_MAJOR_VERSION}::Quick Qt${QT_MAJOR_VERSION}::Qml)
    target_compile_definitions(test_selector PRIVATE QT_COMMANDER_WITH_QML)
endif()
set_target_properties(test_selector PROPERTIES AUTOMOC ON)
add_test(NAME test_selector COMMAND test_selector)
set_tests_properties(test_selector PROPERTIES ENVIRONMENT "PATH=${QT_BIN_PATH}\;$ENV{PATH}")

# --- Handler test (12 tests) ---
add_executable(test_handler
    test_handler.cpp
    handler_test_stubs.cpp
    ${CMAKE_SOURCE_DIR}/src/library/protocol/handler.cpp
    ${CMAKE_SOURCE_DIR}/src/library/core/element_map.cpp
    ${CMAKE_SOURCE_DIR}/src/library/selector/selector.cpp
)
target_include_directories(test_handler PRIVATE ${CMAKE_SOURCE_DIR}/src/library ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_handler PRIVATE Qt${QT_MAJOR_VERSION}::Core Qt${QT_MAJOR_VERSION}::Gui Qt${QT_MAJOR_VERSION}::Widgets)
set_target_properties(test_handler PROPERTIES AUTOMOC ON)
add_test(NAME test_handler COMMAND test_handler)
set_tests_properties(test_handler PROPERTIES ENVIRONMENT "PATH=${QT_BIN_PATH}\;$ENV{PATH}")

# --- RPC server test (parse_utils + screenshot) ---
add_executable(test_rpc_server
    test_rpc_server.cpp
    handler_test_stubs.cpp
    ${CMAKE_SOURCE_DIR}/src/library/protocol/handler.cpp
    ${CMAKE_SOURCE_DIR}/src/library/core/element_map.cpp
    ${CMAKE_SOURCE_DIR}/src/library/selector/selector.cpp
)
target_include_directories(test_rpc_server PRIVATE ${CMAKE_SOURCE_DIR}/src/library ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_rpc_server PRIVATE Qt${QT_MAJOR_VERSION}::Core Qt${QT_MAJOR_VERSION}::Gui Qt${QT_MAJOR_VERSION}::Widgets)
set_target_properties(test_rpc_server PROPERTIES AUTOMOC ON)
add_test(NAME test_rpc_server COMMAND test_rpc_server)
set_tests_properties(test_rpc_server PROPERTIES ENVIRONMENT "PATH=${QT_BIN_PATH}\;$ENV{PATH}")
