import QtQuick 2.12
import QtQuick.Window 2.12

// Occlusion test scene: every layer is a deliberately overlapping element
// with an objectName so the snapshot occlusion solver can be verified
// end-to-end through the injected MCP.
//
// Expected prune outcome (all coordinates window-local):
//   occlA  partially covered by occlB      -> kept, visible_ratio < 1
//   occlB  paints above occlA (same z)     -> kept, ratio 1
//   occlC  fully covered by occlD          -> REMOVED
//   occlD  later sibling, same geometry    -> kept
//   occlE  partially covered by occlF (z)  -> kept, ratio < 1
//   occlF  z=3 paints above occlE z=1      -> kept
//   occlBtnRoot  covered by its own bg     -> REMOVED, bg kept (reparented)
//   occlBtnRoot2 covered by z=0 bg child   -> REMOVED (child z never
//       even though root z=2)                    crosses the subtree layer
//   occlNegParent  covered by its bg       -> REMOVED
//   occlNegBg     covers negative-z child  -> kept
//   occlNegZ      z=-1 paints UNDER parent -> REMOVED (covered by occlNegBg)
//   occlHidden / occlHiddenCover  invisible -> never in the snapshot, never
//       occlude (occlHiddenCover overlaps occlE/occlF)

Window {
    id: occRoot
    width: 640; height: 420
    visible: true
    color: "#f0f0f0"
    objectName: "occlRoot"

    Rectangle { width: 640; height: 420; color: "#e8e8e8"; objectName: "occlBg" }

    // ---- partial overlap: same-z siblings, later paints above ----
    Rectangle { x: 10; y: 10; width: 120; height: 80; color: "red"; objectName: "occlA" }
    Rectangle { x: 40; y: 30; width: 120; height: 80; color: "green"; objectName: "occlB" }

    // ---- full overlap: D covers C completely ----
    Rectangle { x: 180; y: 10; width: 120; height: 80; color: "blue"; objectName: "occlC" }
    Rectangle { x: 180; y: 10; width: 120; height: 80; color: "yellow"; objectName: "occlD" }

    // ---- explicit z: F (z=3) paints above E (z=1) ----
    Rectangle { x: 330; y: 10; width: 120; height: 80; color: "cyan"; objectName: "occlE"; z: 1 }
    Rectangle { x: 350; y: 20; width: 120; height: 80; color: "magenta"; objectName: "occlF"; z: 3 }

    // ---- button-root pattern: transparent root + full-size bg child ----
    Item {
        objectName: "occlBtnRoot"
        x: 10; y: 130; width: 40; height: 40
        Rectangle {
            objectName: "occlBtnBg"
            anchors.fill: parent
            color: "#4CAF50"
        }
    }

    // ---- cross-layer z: root z=2, its bg child z=0 ----
    Item {
        objectName: "occlBtnRoot2"
        x: 70; y: 130; width: 40; height: 40
        z: 2
        Rectangle {
            objectName: "occlBtnBg2"
            anchors.fill: parent
            color: "#2196F3"
            z: 0
        }
    }

    // ---- negative z: paints UNDER the parent's own background ----
    Item {
        objectName: "occlNegParent"
        x: 130; y: 130; width: 120; height: 80
        Rectangle {
            objectName: "occlNegBg"
            anchors.fill: parent
            color: "#90A4AE"
        }
        Rectangle {
            objectName: "occlNegZ"
            anchors.fill: parent
            color: "#FF5722"
            z: -1
        }
    }

    // ---- hidden elements: never appear, never occlude ----
    Rectangle {
        objectName: "occlHidden"
        x: 270; y: 130; width: 150; height: 100
        color: "black"
        visible: false
    }
    Rectangle {
        // overlaps occlE/occlF but is invisible: must not hide them
        objectName: "occlHiddenCover"
        x: 300; y: 0; width: 200; height: 120
        color: "black"
        visible: false
    }

    // ---- visible status text (always kept) ----
    Text {
        objectName: "occlStatus"
        text: "Occlusion test scene"
        color: "#333"
        font.pixelSize: 14
        x: 10; y: 250
    }

    // ---- transparency scenarios (right side) ----
    // Expected:
    //   occlSemiBase    kept (semi-transparent occlSemi does NOT occlude)
    //   occlSemi        kept (it is visible itself)
    //   occlZeroBase    kept; occlZero REMOVED (opacity=0, isVisible true)
    //   occlAlphaBase   kept (fill-alpha rectangle does not occlude)
    //   occlAlphaColor  kept
    //   occlSemiParent  kept; occlSemiChild kept, does NOT occlude
    //   occlClipChild   ratio 0.5 (clip: true cuts the overflow)
    //   occlHalfOut     ratio 0.6 (half outside the window)
    Item {
        objectName: "occlTransparent"
        x: 460; y: 10; width: 170; height: 380

        Rectangle { x: 0; y: 0; width: 120; height: 80; color: "red";
                    objectName: "occlSemiBase" }
        Rectangle { x: 30; y: 20; width: 120; height: 80; color: "blue";
                    opacity: 0.5; objectName: "occlSemi" }

        Rectangle { x: 0; y: 100; width: 120; height: 80; color: "green";
                    objectName: "occlZeroBase" }
        Rectangle { x: 0; y: 100; width: 120; height: 80; color: "black";
                    opacity: 0.0; objectName: "occlZero" }

        Rectangle { x: 0; y: 200; width: 120; height: 80; color: "orange";
                    objectName: "occlAlphaBase" }
        Rectangle { x: 30; y: 215; width: 120; height: 80;
                    color: "#80000000"; objectName: "occlAlphaColor" }

        Item {
            objectName: "occlSemiParent"
            x: 0; y: 300; width: 120; height: 80
            opacity: 0.5
            Rectangle { anchors.fill: parent; color: "purple";
                        objectName: "occlSemiChild" }
        }
    }

    // clip: true cuts the overflowing child to the container rect
    Item {
        objectName: "occlClipBox"
        x: 460; y: 0; width: 100; height: 100
        clip: true
        Rectangle { x: 50; y: 0; width: 100; height: 100; color: "teal";
                    objectName: "occlClipChild" }
    }

    // sticks half out of the window (x = -40): only 60% is on screen
    Rectangle {
        objectName: "occlHalfOut"
        x: -40; y: 330; width: 100; height: 50
        color: "#FF9800"
    }
}
