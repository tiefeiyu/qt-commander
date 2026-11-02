import QtQuick 2.15
import QtQuick.Controls 2.15

ApplicationWindow {
    id: window
    objectName: "qmlWindow"
    width: 640
    height: 480
    visible: true
    title: "QML Window Test"

    Column {
        objectName: "rootColumn"
        anchors.centerIn: parent
        spacing: 10

        Text {
            id: titleLabel
            objectName: "titleLabel"
            text: "QML Window Test"
        }

        TextField {
            id: nameField
            objectName: "nameField"
            text: "initial"
        }

        Rectangle {
            id: actionArea
            objectName: "actionArea"
            width: 120
            height: 40
            color: "#e0e0e0"
            Text {
                anchors.centerIn: parent
                text: "Run"
            }
            MouseArea {
                id: actionMouse
                objectName: "actionMouse"
                anchors.fill: parent
                onClicked: statusLabel.text = "clicked"
            }
        }

        Text {
            id: statusLabel
            objectName: "statusLabel"
            text: "idle"
        }
    }
}
