import QtQuick 2.15
import QtQuick.Controls 2.15

ApplicationWindow {
    id: window
    objectName: "qmlWindow"
    width: 640
    height: 480
    visible: true
    title: "QML Window Test"

    Component {
        id: duplicateComponent
        Rectangle {
            id: duplicateItem
            objectName: "duplicateItem"
            width: 50
            height: 20
            color: "#c0c0c0"
        }
    }

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

        TextField {
            id: hiddenField
            objectName: "hiddenField"
            visible: false
            text: "hidden"
        }

        Loader { sourceComponent: duplicateComponent }
        Loader { sourceComponent: duplicateComponent }
    }
}
