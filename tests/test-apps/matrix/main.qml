import QtQuick 2.15

Rectangle {
    objectName: "qmlRoot"
    width: 320
    height: 200
    color: "#e8f0ff"

    Text {
        objectName: "qmlText"
        anchors.centerIn: parent
        text: "QML content"
        font.pixelSize: 24
    }
}
