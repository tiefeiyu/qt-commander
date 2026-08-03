set(CMAKE_AUTOMOC ON)

# Find Qt
set(QT_COMPONENTS Core Gui Widgets)
if(WITH_QML)
    list(APPEND QT_COMPONENTS Quick QuickWidgets)
    if(QT_MAJOR_VERSION EQUAL 6)
        list(APPEND QT_COMPONENTS Qml)
    endif()
endif()
find_package(Qt${QT_MAJOR_VERSION} REQUIRED COMPONENTS ${QT_COMPONENTS})

set(LIB_SOURCES
    rpc/rpc_server.cpp
    core/ui_scanner.cpp
    core/event_injector.cpp
    core/element_map.cpp
    core/screenshot.cpp
    selector/selector.cpp
    protocol/handler.cpp
    ../common/socket_utils.cpp
)

add_library(qt-commander-lib SHARED ${LIB_SOURCES}
    rpc/rpc_server.h
    protocol/handler.h
)

target_include_directories(qt-commander-lib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ..
)

# Qt base (always required)
target_link_libraries(qt-commander-lib PRIVATE
    Qt${QT_MAJOR_VERSION}::Core
    Qt${QT_MAJOR_VERSION}::Gui
    Qt${QT_MAJOR_VERSION}::Widgets
)

# QML support (optional)
if(WITH_QML)
    target_compile_definitions(qt-commander-lib PRIVATE QT_COMMANDER_WITH_QML)
    target_link_libraries(qt-commander-lib PRIVATE
        Qt${QT_MAJOR_VERSION}::Quick
        Qt${QT_MAJOR_VERSION}::QuickWidgets
    )
    if(QT_MAJOR_VERSION EQUAL 6)
        target_link_libraries(qt-commander-lib PRIVATE Qt6::Qml)
    endif()
endif()

if(WIN32)
    target_link_libraries(qt-commander-lib PRIVATE ws2_32)
endif()

# Platform-specific entry point
if(WIN32)
    target_sources(qt-commander-lib PRIVATE entry_win.cpp)
    set_target_properties(qt-commander-lib PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
else()
    target_sources(qt-commander-lib PRIVATE entry_unix.cpp)
endif()

# Output name
set_target_properties(qt-commander-lib PROPERTIES
    OUTPUT_NAME "qt-commander"
    PREFIX "lib"
)

install(TARGETS qt-commander-lib
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION bin
)
