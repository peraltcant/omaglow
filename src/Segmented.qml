import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Mutually exclusive choices with the current one filled in the accent, so
// the selection reads at a glance. The choices wrap onto more lines when the
// space is narrower than they are.
Rectangle {
    id: control
    property var options: []      // labels
    property var enabledOptions: [] // optional per-option enabled flags
    property int current: -1
    signal picked(int index)

    readonly property real segmentWidth: {
        var widest = 72
        for (var i = 0; i < repeater.count; ++i) {
            var item = repeater.itemAt(i)
            if (item) widest = Math.max(widest, item.labelWidth + 32)
        }
        return widest
    }

    // All on one line when it fits, otherwise as many per line as fit.
    // The extra pixel keeps rounding from pushing the last choice onto a new line.
    implicitWidth: Math.ceil(flow.implicitWidthOneLine) + 5
    implicitHeight: flow.implicitHeight + 4
    Layout.maximumWidth: implicitWidth
    Layout.fillWidth: true
    radius: Math.min(6, theme.radius)
    color: "#0e0e10"
    border.color: "#2e2e35"

    Flow {
        id: flow
        anchors.fill: parent
        anchors.margins: 2
        spacing: 2
        readonly property real implicitWidthOneLine: {
            var total = 0, shown = 0
            for (var i = 0; i < repeater.count; ++i) {
                var item = repeater.itemAt(i)
                if (item && item.visible) { total += control.segmentWidth; ++shown }
            }
            return total + Math.max(0, shown - 1) * spacing
        }
        Repeater {
            id: repeater
            model: control.options
            Rectangle {
                id: segment
                required property int index
                required property string modelData
                readonly property real labelWidth: label.implicitWidth
                readonly property bool usable: control.enabledOptions.length === 0 || control.enabledOptions[index]
                visible: usable
                width: control.segmentWidth
                height: 38
                radius: Math.max(0, Math.min(5, theme.radius) - 1)
                color: index === control.current ? theme.accent : hover.hovered ? "#22222a" : "transparent"
                Label {
                    id: label
                    anchors.centerIn: parent
                    text: segment.modelData
                    font.pixelSize: 14
                    color: segment.index === control.current ? theme.accentForeground : "white"
                }
                HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: control.picked(segment.index) }
            }
        }
    }
}
