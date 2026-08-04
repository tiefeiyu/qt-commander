cmake_minimum_required(VERSION 3.16)

add_executable(qt-injector
    main.cpp
    injector_win.cpp
    ../common/socket_utils.cpp
)

target_include_directories(qt-injector PRIVATE
    ../common
    ../library
)

target_compile_features(qt-injector PRIVATE cxx_std_17)

if(WIN32)
    # Win32 import libs work for both MSVC and MinGW (mingw-w64 provides
    # psapi/bcrypt/ws2_32 as import libraries).
    target_link_libraries(qt-injector PRIVATE psapi bcrypt ws2_32)
    if(MSVC)
        target_compile_options(qt-injector PRIVATE /utf-8)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
           CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
        # GCC < 9 (Qt 5's MinGW 8.1): std::filesystem lives in a separate
        # library; GCC 9+ links it implicitly.
        target_link_libraries(qt-injector PRIVATE stdc++fs)
    endif()
elseif(UNIX)
    target_link_libraries(qt-injector PRIVATE pthread dl)
endif()

set_target_properties(qt-injector PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

install(TARGETS qt-injector RUNTIME DESTINATION bin)
