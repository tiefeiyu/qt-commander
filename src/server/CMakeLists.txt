set(SOURCES
    mcp/transport_stdio.cpp
    mcp/transport_http.cpp
    mcp/protocol.cpp
    session/session.cpp
    session/session_manager.cpp
    build/build_manager.cpp
    ../common/socket_utils.cpp
    ../common/framing.cpp
)

add_executable(qt-commander
    main.cpp
    ${SOURCES}
)

# Platform-specific sources
if(WIN32)
    target_sources(qt-commander PRIVATE
        inject/injector_win.cpp
        process/process_detector_win.cpp
    )
elseif(APPLE)
    target_sources(qt-commander PRIVATE
        inject/injector_macos.cpp
        process/process_detector_macos.cpp
    )
else()
    target_sources(qt-commander PRIVATE
        inject/injector_linux.cpp
        process/process_detector_linux.cpp
    )
endif()

target_include_directories(qt-commander PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/external
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(qt-commander PRIVATE
    Threads::Threads
)

if(WIN32)
    target_link_libraries(qt-commander PRIVATE ws2_32)
endif()
