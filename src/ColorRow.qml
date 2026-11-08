import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A color: its swatch, a hex field and a palette to pick from.
ColumnLayout {
    id: row
    property string label
    property color value
    // The palette Studio and Devices share.
    property var swatches: ["#ffffff", "#ff0000", "#ff7a00", "#ffd60a", "#00ff00", "#00ffd0",
                            "#00a2ff", "#0000ff", "#8a2bff", "#ff00ff", "#ff2d6f", "#000000"]
    signal chosen(color c)
    Layout.fillWidth: true
    spacing: 8
    RowLayout {
        spacing: 12
        Rectangle {
            width: 34; height: 34
            radius: Math.min(8, theme.radius)
            color: row.value
            border.color: "#3a3a42"
        }
        Label { text: row.label; Layout.fillWidth: true }
        TextField {
            Layout.preferredWidth: 120
            placeholderText: qsTr("Hex")
            text: row.value.toString()
            validator: RegularExpressionValidator { regularExpression: /#?[0-9a-fA-F]{6}/ }
            onEditingFinished: row.chosen(text.charAt(0) === "#" ? text : "#" + text)
        }
    }
    GridLayout {
        Layout.fillWidth: true
        columns: 12
        columnSpacing: 6
        rowSpacing: 6
        Repeater {
            model: row.swatches
            Rectangle {
                required property string modelData
                readonly property bool picked: row.value.toString() === Qt.color(modelData).toString()
                Layout.fillWidth: true
                implicitHeight: 28
                radius: Math.min(6, theme.radius)
                color: modelData
                border.width: picked ? 3 : 1
                border.color: picked ? theme.accent : "#33333a"
                HoverHandler { cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: row.chosen(parent.modelData) }
            }
        }
    }
}
