import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A titled card that groups related settings.
Rectangle {
    id: section
    property string title
    property string note
    property string icon // a bundled Lucide name, shown before the title
    default property alias content: body.data

    Layout.fillWidth: true
    implicitHeight: column.implicitHeight + 40
    radius: Math.min(10, theme.radius)
    color: "#17171b"
    border.color: "#232329"

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: 20
        spacing: 14

        RowLayout {
            visible: section.title !== ""
            spacing: 8
            Icon {
                visible: section.icon !== ""
                glyph: section.icon
                size: 15
                opacity: 0.55
            }
            Label {
                text: section.title.toUpperCase()
                font.pixelSize: 12
                font.bold: true
                font.letterSpacing: 1
                opacity: 0.55
            }
        }
        ColumnLayout {
            id: body
            Layout.fillWidth: true
            spacing: 16
        }
        Label {
            visible: section.note !== ""
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: section.note
            font.pixelSize: 13
            opacity: 0.5
        }
    }
}
