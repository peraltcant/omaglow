import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ScrollView {
    id: page
    contentWidth: availableWidth
    // Labels sit above their choices when there's no room beside them.
    readonly property bool narrow: availableWidth < 760

    ColumnLayout {
        // Rows stay readable on wide windows: the label and its control together.
        width: Math.min(page.availableWidth, 1000)
        spacing: 16

        Label { text: qsTr("Power"); font.pixelSize: 26; font.bold: true }

        Section {
            icon: "battery"
            title: qsTr("Battery")
            // Armoury lists these under its wireless power saving options; on
            // the cable the keyboard is powered and stayed awake in testing.
            note: qsTr("Armoury lists these as wireless power options. On the cable the keyboard doesn't go to sleep.")

            GridLayout {
                Layout.fillWidth: true
                columns: page.narrow ? 1 : 2
                columnSpacing: 16
                rowSpacing: 8
                Label { text: qsTr("Sleep after"); Layout.preferredWidth: 160 }
                Segmented {
                    // Index order the keyboard uses, then never.
                    readonly property var values: [0, 1, 2, 3, 4, 255]
                    options: {
                        translator.language
                        return [qsTr("%n min", "", 1), qsTr("%n min", "", 2), qsTr("%n min", "", 3),
                                qsTr("%n min", "", 5), qsTr("%n min", "", 10), qsTr("Never")]
                    }
                    current: values.indexOf(backend.sleepTimeout)
                    onPicked: (i) => backend.sleepTimeout = values[i]
                }
            }
            GridLayout {
                Layout.fillWidth: true
                columns: page.narrow ? 1 : 2
                columnSpacing: 16
                rowSpacing: 8
                Label { text: qsTr("Low battery alert"); Layout.preferredWidth: 160 }
                Segmented {
                    readonly property var values: [10, 20, 30, 40, 50]
                    options: values.map(v => qsTr("%1%").arg(v.toLocaleString(Qt.locale(), 'f', 0)))
                    current: values.indexOf(backend.batteryAlert)
                    onPicked: (i) => backend.batteryAlert = values[i]
                }
            }
            GridLayout {
                Layout.fillWidth: true
                columns: page.narrow ? 1 : 2
                columnSpacing: 16
                rowSpacing: 8
                Label { text: qsTr("Power saving"); Layout.preferredWidth: 160 }
                Segmented {
                    options: [qsTr("Off"), qsTr("Lights off"), qsTr("Dim lights")]
                    current: backend.powerSaving
                    onPicked: (i) => backend.powerSaving = i
                }
            }
        }

        Section {
            icon: "timer"
            title: qsTr("Typing")
            note: qsTr("A longer debounce time ignores switch chatter but can miss very fast repeated presses.")
            GridLayout {
                Layout.fillWidth: true
                columns: page.narrow ? 1 : 2
                columnSpacing: 16
                rowSpacing: 8
                Label { text: qsTr("Debounce time (ms)"); Layout.preferredWidth: 160 }
                Segmented {
                    readonly property var values: [15, 20, 25, 30, 35, 40, 45, 50]
                    options: values.map(v => v.toLocaleString(Qt.locale(), 'f', 0))
                    current: values.indexOf(backend.debounceMs)
                    onPicked: (i) => backend.debounceMs = values[i]
                }
            }
        }
        Item { Layout.preferredHeight: 4 }
    }
}
