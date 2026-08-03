import QtQuick 2.12
import QtQuick.Window 2.12

Window {
    id: qmlRoot
    width: 800; height: 600; color: "#f0f0f0"
    visible: true
    objectName: "qmlRoot"

    // Operation log: every interaction appends a line here so a human (or
    // a screenshot) can see exactly what the MCP did.
    property var logModel: ListModel {}
    function log(msg) {
        logModel.insert(0, {
            entry: new Date().toLocaleTimeString() + " " + msg
        });
    }

    Rectangle {
        id: logPanel
        width: 230; color: "#1e1e1e"; radius: 6
        anchors { top: parent.top; topMargin: 20; right: parent.right; rightMargin: 20; bottom: parent.bottom; bottomMargin: 20 }
        objectName: "logPanel"
        border.color: "#444"
        Text {
            text: "Operation log"
            color: "#888"; font.pixelSize: 12
            anchors { top: parent.top; topMargin: 6; horizontalCenter: parent.horizontalCenter }
        }
        ListView {
            id: logList
            anchors { top: parent.top; topMargin: 26; left: parent.left; right: parent.right; bottom: parent.bottom; margins: 6 }
            model: logModel
            spacing: 2
            delegate: Text {
                text: model.entry
                color: "#dcdcdc"; font.pixelSize: 11
                wrapMode: Text.Wrap
                width: logList.width
            }
        }
    }

    Text {
        id: titleText
        text: "QML Test Application"
        font.pixelSize: 24
        anchors { top: parent.top; topMargin: 20; horizontalCenter: parent.horizontalCenter }
        objectName: "titleText"
    }

    // ---- OK Button ----
    Rectangle {
        id: btnOK
        width: 100; height: 40; color: "#4CAF50"; radius: 4
        anchors { top: titleText.bottom; topMargin: 20; left: parent.left; leftMargin: 80 }
        objectName: "btnOK"
        Text { anchors.centerIn: parent; text: "OK"; color: "white"; font.pixelSize: 16 }
        MouseArea {
            anchors.fill: parent
            objectName: "btnOKMouseArea"
            onClicked: {
                statusText.text = "OK clicked!"; statusText.color = "green";
                qmlRoot.log("OK button clicked");
            }
        }
    }

    // ---- Cancel Button ----
    Rectangle {
        id: btnCancel
        width: 100; height: 40; color: "#f44336"; radius: 4
        anchors { top: titleText.bottom; topMargin: 20; left: btnOK.right; leftMargin: 10 }
        objectName: "btnCancel"
        Text { anchors.centerIn: parent; text: "Cancel"; color: "white"; font.pixelSize: 16 }
        MouseArea {
            anchors.fill: parent
            objectName: "btnCancelMouseArea"
            onClicked: {
                statusText.text = "Cancel clicked!"; statusText.color = "red";
                qmlRoot.log("Cancel button clicked");
            }
        }
    }

    // ---- Text input ----
    Rectangle {
        id: inputBox
        width: 300; height: 40; color: "white"; border.color: "#ccc"; border.width: 1; radius: 4
        anchors { top: btnOK.bottom; topMargin: 20; horizontalCenter: parent.horizontalCenter }
        objectName: "inputBox"
        TextInput {
            id: textInput
            anchors { fill: parent; margins: 8 }
            font.pixelSize: 16
            text: ""
            objectName: "textInput"
            onTextChanged: {
                statusText.text = "Input: " + text; statusText.color = "gray";
                if (text.length > 0)
                    qmlRoot.log("Input: " + text);
            }
        }
    }

    // ---- Click target (counts clicks) ----
    Rectangle {
        id: clickTarget
        width: 200; height: 50; color: "#2196F3"; radius: 4
        anchors { top: inputBox.bottom; topMargin: 20; horizontalCenter: parent.horizontalCenter }
        objectName: "clickTarget"
        property int clickCount: 0
        Text { anchors.centerIn: parent; text: "Click Me"; color: "white"; font.pixelSize: 16 }
        MouseArea {
            anchors.fill: parent
            objectName: "clickTargetMouseArea"
            onClicked: {
                clickTarget.clickCount++;
                statusText.text = "Target clicked " + clickTarget.clickCount + "x";
                statusText.color = "blue";
                qmlRoot.log("Target clicked " + clickTarget.clickCount + "x");
            }
        }
    }

    // ---- Status ----
    Text {
        id: statusText
        text: "Status: Ready"
        font.pixelSize: 18; color: "gray"
        anchors { top: clickTarget.bottom; topMargin: 20; horizontalCenter: parent.horizontalCenter }
        objectName: "statusText"
    }

    // ---- Toggle switch ----
    Rectangle {
        id: toggleSwitch
        width: 80; height: 30; color: "#9E9E9E"; radius: 15
        anchors { top: statusText.bottom; topMargin: 20; left: parent.left; leftMargin: 120 }
        objectName: "toggleSwitch"
        property bool on: false
        Text {
            anchors.centerIn: parent
            text: toggleSwitch.on ? "ON" : "OFF"
            color: "white"; font.pixelSize: 14
        }
        MouseArea {
            anchors.fill: parent
            objectName: "toggleSwitchMouseArea"
            onClicked: {
                toggleSwitch.on = !toggleSwitch.on;
                toggleSwitch.color = toggleSwitch.on ? "#4CAF50" : "#9E9E9E";
                qmlRoot.log("Toggle switched to " + (toggleSwitch.on ? "ON" : "OFF"));
            }
        }
    }

    // ---- Progress bar (click to fill) ----
    Rectangle {
        id: progressBox
        width: 220; height: 30; color: "white"; border.color: "#ccc"; radius: 4
        anchors { top: statusText.bottom; topMargin: 20; left: toggleSwitch.right; leftMargin: 30 }
        objectName: "progressBox"
        property real progress: 0.2
        Rectangle {
            id: progressFill
            width: progressBox.width * progressBox.progress; height: 30
            color: "#FF9800"; radius: 4
            objectName: "progressFill"
        }
        MouseArea {
            anchors.fill: parent
            objectName: "progressMouseArea"
            onClicked: {
                progressBox.progress = Math.min(1.0, progressBox.progress + 0.2);
                qmlRoot.log("Progress +20% -> " + Math.round(progressBox.progress * 100) + "%");
            }
        }
    }

    // ---- Animated box (color pulse on click) ----
    Rectangle {
        id: animatedBox
        width: 60; height: 60; color: "#9C27B0"; radius: 8
        anchors { top: statusText.bottom; topMargin: 20; left: progressBox.right; leftMargin: 30 }
        objectName: "animatedBox"
        property bool pulsing: false
        ColorAnimation on color {
            id: pulseAnim
            from: "#9C27B0"; to: "#FFEB3B"
            duration: 400
            running: animatedBox.pulsing
            onStopped: animatedBox.color = "#9C27B0"
        }
        MouseArea {
            anchors.fill: parent
            objectName: "animatedBoxMouseArea"
            onClicked: { animatedBox.pulsing = true; }
        }
    }

    // ---- List view ----
    ListView {
        id: listView
        width: 300; height: 120
        anchors { top: toggleSwitch.bottom; topMargin: 20; horizontalCenter: parent.horizontalCenter }
        model: ListModel {
            ListElement { name: "Item A" }
            ListElement { name: "Item B" }
            ListElement { name: "Item C" }
        }
        objectName: "listView"
        delegate: Rectangle {
            width: 300; height: 40; color: (index % 2 == 0) ? "#eee" : "#fff"
            objectName: "listItem" + index
            Text { anchors.centerIn: parent; text: model.name; font.pixelSize: 14 }
            MouseArea {
                anchors.fill: parent
                objectName: "listItemMouseArea" + index
                onClicked: {
                    statusText.text = "List item " + model.name + " selected";
                    statusText.color = "purple";
                    qmlRoot.log("List item " + model.name + " selected");
                }
            }
        }
    }
}
