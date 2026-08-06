set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# ---- QWidget occlusion app (no QML dependency) ------------------------
add_executable(qt-occlusion-widget-test main_widget.cpp)
target_link_libraries(qt-occlusion-widget-test PRIVATE
    Qt${QT_MAJOR_VERSION}::Core
    Qt${QT_MAJOR_VERSION}::Gui
    Qt${QT_MAJOR_VERSION}::Widgets
)
set_target_properties(qt-occlusion-widget-test PROPERTIES
    WIN32_EXECUTABLE TRUE)
install(TARGETS qt-occlusion-widget-test RUNTIME DESTINATION bin
        COMPONENT occlusion-tests)

# ---- QML occlusion app (needs WITH_QML) --------------------------------
if(WITH_QML)
    add_executable(qt-occlusion-qml-test
        main_qml.cpp
        occlusion_scene.qml
        resources.qrc
    )
    target_link_libraries(qt-occlusion-qml-test PRIVATE
        Qt${QT_MAJOR_VERSION}::Core
        Qt${QT_MAJOR_VERSION}::Gui
        Qt${QT_MAJOR_VERSION}::Quick
    )
    set_target_properties(qt-occlusion-qml-test PROPERTIES
        WIN32_EXECUTABLE TRUE)

    # Copy QML file to output directory for runtime loading
    add_custom_command(TARGET qt-occlusion-qml-test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/occlusion_scene.qml"
            "$<TARGET_FILE_DIR:qt-occlusion-qml-test>/occlusion_scene.qml"
    )
    install(TARGETS qt-occlusion-qml-test RUNTIME DESTINATION bin
            COMPONENT occlusion-tests)
    install(FILES occlusion_scene.qml DESTINATION bin
            COMPONENT occlusion-tests)
endif()

# Deploy Qt runtime DLLs next to the exes so the apps run standalone
# right after `cmake --install`.
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
                \"\$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/qt-occlusion-widget-test.exe\"
                RESULT_VARIABLE _wdq_result)
            if(NOT _wdq_result EQUAL 0)
                message(WARNING \"windeployqt failed (exit \${_wdq_result})\")
            endif()
        " COMPONENT occlusion-tests)
        if(WITH_QML)
            install(CODE "
                execute_process(COMMAND \"${WINDEPLOYQT_EXE}\"
                    --release --no-translations --no-compiler-runtime
                    --qmldir \"${CMAKE_CURRENT_SOURCE_DIR}\"
                    \"\$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/qt-occlusion-qml-test.exe\"
                    RESULT_VARIABLE _wdq_result)
                if(NOT _wdq_result EQUAL 0)
                    message(WARNING \"windeployqt failed (exit \${_wdq_result})\")
                endif()
            " COMPONENT occlusion-tests)
        endif()
    else()
        message(WARNING
            "windeployqt not found; Qt DLLs must be deployed manually")
    endif()
endif()
