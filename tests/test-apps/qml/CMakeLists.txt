set(CMAKE_AUTOMOC ON)

add_executable(qt-qml-test main.cpp test_scene.qml)

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
