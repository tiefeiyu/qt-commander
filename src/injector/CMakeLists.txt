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

if(MSVC)
    target_compile_options(qt-injector PRIVATE /utf-8)
    target_link_libraries(qt-injector PRIVATE psapi bcrypt ws2_32)
elseif(UNIX)
    target_link_libraries(qt-injector PRIVATE pthread dl)
endif()

set_target_properties(qt-injector PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

install(TARGETS qt-injector RUNTIME DESTINATION bin)
