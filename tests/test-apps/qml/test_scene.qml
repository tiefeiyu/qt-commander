import QtQuick 2.12
import QtQuick.Window 2.12

Window {
    width: 800; height: 600; color: "#f0f0f0"
    visible: true
    objectName: "qmlRoot"

    Text {
        id: titleText
        text: "QML Test Application"
        font.pixelSize: 24
        anchors { top: parent.top; topMargin: 20; horizontalCenter: parent.horizontalCenter }
        objectName: "titleText"
    }

    // OK Button
    Rectangle {
        id: btnOK
        width: 100; height: 40; color: "#4CAF50"; radius: 4
        anchors { top: titleText.bottom; topMargin: 20; left: parent.left; leftMargin: 80 }
        objectName: "btnOK"
        Text { anchors.centerIn: parent; text: "OK"; color: "white"; font.pixelSize: 16 }
        MouseArea {
            anchors.fill: parent
            objectName: "btnOKMouseArea"
            onClicked: { statusText.text = "OK clicked!"; statusText.color = "green"; }
        }
    }

    // Cancel Button
    Rectangle {
        id: btnCancel
        width: 100; height: 40; color: "#f44336"; radius: 4
        anchors { top: titleText.bottom; topMargin: 20; left: btnOK.right; leftMargin: 10 }
        objectName: "btnCancel"
        Text { anchors.centerIn: parent; text: "Cancel"; color: "white"; font.pixelSize: 16 }
        MouseArea {
            anchors.fill: parent
            objectName: "btnCancelMouseArea"
            onClicked: { statusText.text = "Cancel clicked!"; statusText.color = "red"; }
        }
    }

    // Text input
    Rectangle {
        width: 300; height: 40; color: "white"; border.color: "#ccc"; border.width: 1; radius: 4
        anchors { top: btnOK.bottom; topMargin: 20; horizontalCenter: parent.horizontalCenter }
        TextInput {
            id: textInput
            anchors { fill: parent; margins: 8 }
            font.pixelSize: 16
            text: ""
            objectName: "textInput"
        }
    }

    // Click target
    Rectangle {
        id: clickTarget
        width: 200; height: 50; color: "#2196F3"; radius: 4
        anchors { top: textInput.bottom; topMargin: 20; horizontalCenter: parent.horizontalCenter }
        objectName: "clickTarget"
        Text { anchors.centerIn: parent; text: "Click Me"; color: "white"; font.pixelSize: 16 }
        MouseArea {
            anchors.fill: parent
            objectName: "clickTargetMouseArea"
            onClicked: { statusText.text = "Target clicked!"; statusText.color = "blue"; }
        }
    }

    // Status
    Text {
        id: statusText
        text: "Status: Ready"
        font.pixelSize: 18; color: "gray"
        anchors { top: clickTarget.bottom; topMargin: 20; horizontalCenter: parent.horizontalCenter }
        objectName: "statusText"
    }

    // ListView
    ListView {
        id: listView
        width: 300; height: 120
        anchors { top: statusText.bottom; topMargin: 20; horizontalCenter: parent.horizontalCenter }
        model: ListModel {
            ListElement { name: "Item A" }
            ListElement { name: "Item B" }
            ListElement { name: "Item C" }
        }
        objectName: "listView"
        delegate: Rectangle {
            width: 300; height: 40; color: (index % 2 == 0) ? "#eee" : "#fff"
            Text { anchors.centerIn: parent; text: model.name; font.pixelSize: 14 }
        }
    }
}
