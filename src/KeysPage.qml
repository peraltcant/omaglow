import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ScrollView {
    id: page
    contentWidth: availableWidth
    property int selected: 0x1e
    readonly property var selectedKey: {
        for (var i = 0; i < backend.keys.length; ++i)
            if (backend.keys[i].code === selected) return backend.keys[i]
        return null
    }

    ColumnLayout {
        // Rows stay readable on wide windows: the label and its control together.
        width: Math.min(page.availableWidth, 1250)
        spacing: 16

        RowLayout {
            Label { text: qsTr("Keys"); font.pixelSize: 26; font.bold: true; Layout.fillWidth: true }
            Label { opacity: 0.6; text: qsTr("%n keys changed", "", backend.remappedCount) }
            Button {
                text: qsTr("Reset all keys"); icon.source: "qrc:/icons/rotate-ccw.svg"
                flat: true
                enabled: backend.remappedCount > 0
                onClicked: resetDialog.open()
            }
        }

        Section {
            icon: "keyboard"
            title: qsTr("Pick a key")
            note: qsTr("Changes are stored on the keyboard and work on any computer.")

            // The ANSI 75% layout, drawn in key units.
            Item {
                id: board
                Layout.fillWidth: true
                readonly property real unit: width / 16.25
                Layout.preferredHeight: unit * 6.25
                Repeater {
                    model: backend.keys
                    Rectangle {
                        required property var modelData
                        readonly property bool chosen: modelData.code === page.selected
                        x: modelData.x * board.unit + 2
                        y: modelData.y * board.unit + 2
                        width: modelData.width * board.unit - 4
                        height: board.unit - 4
                        radius: Math.min(4, theme.radius)
                        color: chosen ? theme.accent : hover.hovered ? "#26262d" : modelData.remapped ? "#24242b" : "#1d1d22"
                        border.color: chosen || modelData.remapped ? theme.accent : "#2a2a31"
                        Column {
                            anchors.centerIn: parent
                            Label {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: modelData.label
                                font.pixelSize: Math.max(9, board.unit * 0.24)
                                color: chosen ? theme.accentForeground : "white"
                            }
                            Label {
                                anchors.horizontalCenter: parent.horizontalCenter
                                visible: modelData.remapped
                                width: Math.min(implicitWidth, modelData.width * board.unit - 8)
                                elide: Text.ElideRight
                                text: modelData.action
                                font.pixelSize: Math.max(8, board.unit * 0.17)
                                color: chosen ? theme.accentForeground : theme.accent
                            }
                        }
                        HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: page.selected = modelData.code }
                    }
                }
            }
        }

        Section {
            icon: "pencil"
            title: page.selectedKey ? qsTr("%1 key").arg(page.selectedKey.label) : ""
            GridLayout {
                // One per line when the window is narrow.
                columns: page.availableWidth < 700 ? 1 : 3
                columnSpacing: 16
                rowSpacing: 8
                Label { text: qsTr("Sends"); Layout.preferredWidth: 110 }
                ComboBox {
                    Layout.preferredWidth: 260
                    Layout.maximumWidth: page.availableWidth - 80
                    model: backend.keyChoices
                    textRole: "label"
                    currentIndex: win.choiceIndex(page.selectedKey ? page.selectedKey.actionCode : 255,
                                                  page.selectedKey ? page.selectedKey.actionParam : 0)
                    onActivated: (i) => backend.remap(page.selected, backend.keyChoices[i].code, backend.keyChoices[i].param)
                }
                Button {
                    text: qsTr("Back to default"); icon.source: "qrc:/icons/undo-2.svg"
                    flat: true
                    enabled: page.selectedKey && page.selectedKey.remapped
                    onClicked: backend.remap(page.selected, 255, 0)
                }
            }
        }
        Item { Layout.preferredHeight: 4 }
    }

    Dialog {
        id: resetDialog
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Reset all keys?")
        standardButtons: Dialog.Cancel | Dialog.Reset
        Label { text: qsTr("Every key goes back to what it normally types.") }
        onReset: { backend.resetKeys(); close() }
    }
}
