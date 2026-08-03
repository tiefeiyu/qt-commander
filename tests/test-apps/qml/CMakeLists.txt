set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# resources.qrc embeds test_scene.qml so the app loads from qrc:/
add_executable(qt-qml-test main.cpp test_scene.qml resources.qrc)

target_link_libraries(qt-qml-test PRIVATE
    Qt${QT_MAJOR_VERSION}::Core
    Qt${QT_MAJOR_VERSION}::Gui
    Qt${QT_MAJOR_VERSION}::Quick
)
set_target_properties(qt-qml-test PROPERTIES WIN32_EXECUTABLE TRUE)

# Copy QML file to output directory for runtime loading
add_custom_command(TARGET qt-qml-test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/test_scene.qml"
        "$<TARGET_FILE_DIR:qt-qml-test>/test_scene.qml"
)

# Component-scoped so a widget-only build tree can install just its own app:
#   cmake --install <build> --component qml-tests
install(TARGETS qt-qml-test RUNTIME DESTINATION bin COMPONENT qml-tests)
install(FILES test_scene.qml DESTINATION bin COMPONENT qml-tests)

# Deploy Qt runtime DLLs and QML modules next to the exe so the app runs
# standalone right after `cmake --install`.
if(WIN32)
    if(TARGET Qt${QT_MAJOR_VERSION}::qmake)
        get_target_property(QT_QMAKE Qt${QT_MAJOR_VERSION}::qmake IMPORTED_LOCATION)
    endif()
    if(NOT QT_QMAKE)
        set(QT_QMAKE "${QT_QMAKE_EXECUTABLE}")
    endif()
    if(QT_QMAKE)
        get_filename_component(QT_BIN_DIR "${QT_QMAKE}" DIRECTORY)
        set(WINDEPLOYQT_EXE "${QT_BIN_DIR}/windeployqt.exe")
    endif()
    if(WINDEPLOYQT_EXE AND EXISTS "${WINDEPLOYQT_EXE}")
        install(CODE "
            execute_process(COMMAND \"${WINDEPLOYQT_EXE}\"
                --release --no-translations --no-compiler-runtime
                --qmldir \"${CMAKE_CURRENT_SOURCE_DIR}\"
                \"\$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/qt-qml-test.exe\"
                RESULT_VARIABLE _wdq_result)
            if(NOT _wdq_result EQUAL 0)
                message(WARNING \"windeployqt failed (exit \${_wdq_result})\")
            endif()
        " COMPONENT qml-tests)
    else()
        message(WARNING
            "windeployqt not found; Qt DLLs must be deployed manually")
    endif()
endif()
