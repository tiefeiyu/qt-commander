add_executable(qtc-inject main_native.cpp)

if(MSVC)
    target_compile_options(qtc-inject PRIVATE /utf-8)
endif()

if(WIN32)
    target_sources(qtc-inject PRIVATE injector_win_native.cpp)
    if(MINGW)
        target_link_options(qtc-inject PRIVATE
            -static-libgcc -static-libstdc++
        )
    endif()
elseif(UNIX)
    target_sources(qtc-inject PRIVATE
        injector_linux.cpp
        ptrace_ops.cpp
        elf_parser.cpp
        elf_loader.cpp
    )
    target_link_libraries(qtc-inject PRIVATE pthread dl)
endif()

set_target_properties(qtc-inject PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/qt-commander"
)
