import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ScrollView {
    id: page
    contentWidth: availableWidth
    readonly property bool customOn: backend.knobFunctions.length > 5 && backend.knobFunctions[5].enabled

    ColumnLayout {
        // Rows stay readable on wide windows: the label and its control together.
        width: Math.min(page.availableWidth, 820)
        spacing: 16

        Label { text: qsTr("Control knob"); font.pixelSize: 26; font.bold: true }

        Section {
            icon: "circle-dot"
            title: qsTr("Functions")
            note: qsTr("Turn off the ones you don't want the knob to switch between.")
            Repeater {
                model: backend.knobFunctions
                RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    Label { text: modelData.name; Layout.fillWidth: true }
                    Switch {
                        checked: modelData.enabled
                        onToggled: backend.setKnobFunction(modelData.id, checked)
                    }
                }
            }
        }

        Section {
            icon: "settings-2"
            title: qsTr("Custom function")
            visible: page.customOn
            Repeater {
                model: page.customOn ? backend.knobSlots : []
                RowLayout {
                    required property var modelData
                    spacing: 16
                    Label { text: modelData.name; Layout.preferredWidth: 110 }
                    ComboBox {
                        Layout.preferredWidth: 260
                        model: backend.keyChoices
                        textRole: "label"
                        currentIndex: win.choiceIndex(modelData.code, modelData.param)
                        onActivated: (i) => backend.setKnobSlot(modelData.slot, backend.keyChoices[i].code, backend.keyChoices[i].param)
                    }
                }
            }
        }
        Item { Layout.preferredHeight: 4 }
    }
}
