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
    QT_COMMANDER_VERSION="${QTC_VERSION}"
)
if(MSVC)
    target_compile_options(qt_commander PRIVATE /utf-8)
endif()

target_link_libraries(qt_commander
    PUBLIC Qt${QT_MAJOR_VERSION}::Core Qt${QT_MAJOR_VERSION}::Gui Qt${QT_MAJOR_VERSION}::Widgets Qt${QT_MAJOR_VERSION}::Network Qt${QT_MAJOR_VERSION}::Quick Qt${QT_MAJOR_VERSION}::QuickWidgets
)

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
elseif(UNIX AND NOT APPLE)
    add_library(qt_commander_inject SHARED library/inject_main_unix.cpp)
    target_link_libraries(qt_commander_inject PRIVATE qt_commander)
    set_target_properties(qt_commander_inject PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
endif()

if(WIN32 OR (UNIX AND NOT APPLE))
    add_subdirectory(injector)
endif()


