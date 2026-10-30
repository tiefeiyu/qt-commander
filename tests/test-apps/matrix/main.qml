import QtQuick 2.15

Rectangle {
    id: qmlRoot
    objectName: "qmlRoot"
    width: 320
    height: 200
    color: "#e8f0ff"

    Text {
        id: qmlText
        objectName: "qmlText"
        anchors.centerIn: parent
        text: "QML content"
        font.pixelSize: 24
    }

    Text {
        id: qmlStatus
        objectName: "qmlStatus"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        text: "idle"
        font.pixelSize: 16
    }

    MouseArea {
        id: qmlMouse
        objectName: "qmlMouse"
        anchors.fill: parent
        onClicked: qmlStatus.text = "clicked"
    }
}
