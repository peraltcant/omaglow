import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Every peripheral Omaglow drives besides the keyboard; for now the Razer
// DeathAdder V2's logo and scroll wheel.
ScrollView {
    id: page
    contentWidth: availableWidth
    readonly property bool mouseHere: backend.mouseStatus === 2
    readonly property var zones: [{led: 4, name: qsTr("Logo")}, {led: 1, name: qsTr("Scroll wheel")}]

    component ZoneCard: Section {
        id: card
        property int led
        // What is stored for the zone, or the mouse's default until set.
        readonly property var zone: backend.mouseZones[String(led)]
            || ({effect: 1, color1: "#00ff00", color2: "#0000ff", twoColors: false, brightness: 100})
        function set(changes) {
            const z = Object.assign({}, card.zone, changes)
            backend.setMouseZone(card.led, z.effect, z.color1, z.color2, z.twoColors, z.brightness)
        }
        Layout.fillWidth: true
        Layout.alignment: Qt.AlignTop
        enabled: page.mouseHere

        Segmented {
            options: [qsTr("Off"), qsTr("Static"), qsTr("Breathing"), qsTr("Spectrum")]
            current: card.zone.effect
            onPicked: (i) => card.set({effect: i})
        }
        Segmented {
            visible: card.zone.effect === 2
            options: [qsTr("One color"), qsTr("Two colors")]
            current: card.zone.twoColors ? 1 : 0
            onPicked: (i) => card.set({twoColors: i === 1})
        }
        ColorRow {
            visible: card.zone.effect === 1 || card.zone.effect === 2
            label: qsTr("Color")
            value: card.zone.color1
            onChosen: (c) => card.set({color1: c})
        }
        ColorRow {
            visible: card.zone.effect === 2 && card.zone.twoColors
            label: qsTr("Second color")
            value: card.zone.color2
            onChosen: (c) => card.set({color2: c})
        }
        ColumnLayout {
            visible: card.zone.effect !== 0
            Layout.fillWidth: true
            spacing: 8
            RowLayout {
                Label { text: qsTr("Brightness"); Layout.fillWidth: true }
                Label { text: qsTr("%1%").arg(Math.round(brightness.value)); font.bold: true }
            }
            // Sent when let go, not on every step of the drag.
            Slider {
                id: brightness
                Layout.fillWidth: true
                from: 0; to: 100; stepSize: 1
                value: card.zone.brightness
                onPressedChanged: if (!pressed) card.set({brightness: Math.round(value)})
            }
        }
    }

    ColumnLayout {
        width: Math.min(page.availableWidth, 1250)
        spacing: 16

        Label { text: qsTr("Devices"); font.pixelSize: 26; font.bold: true }

        Section {
            title: qsTr("Omarchy theme")
            icon: "palette"
            note: qsTr("When the theme changes, the keyboard, mouse and laptop take its keyboard color, even with Omaglow closed. Changing a device by hand keeps working until the next theme.")
            RowLayout {
                spacing: 12
                Rectangle {
                    width: 28; height: 28
                    radius: Math.min(6, theme.radius)
                    color: theme.keyboardColor
                    border.color: "#3a3a42"
                }
                Label { text: qsTr("Follow the Omarchy theme"); Layout.fillWidth: true }
                Switch { checked: backend.followTheme; onToggled: backend.followTheme = checked }
            }
        }

        Section {
            title: qsTr("Razer DeathAdder V2")
            icon: "mouse"
            note: qsTr("Effects are stored on the mouse, so they stay on any computer. Looks saved from here on include them.")
            RowLayout {
                spacing: 8
                Icon {
                    size: 16
                    glyph: page.mouseHere ? "plug" : "circle-alert"
                    color: page.mouseHere ? "#3ddc84" : "#ffb020"
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: page.mouseHere ? qsTr("Connected")
                        : backend.mouseStatus === 1
                          ? qsTr("Found, but Omaglow can't open it. Reinstall Omaglow so its udev rule applies, or replug the mouse.")
                          : qsTr("Not found. Plug the mouse in with its cable.")
                }
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: page.availableWidth > 1050 ? 2 : 1
            columnSpacing: 20
            rowSpacing: 16
            Repeater {
                model: page.zones
                ZoneCard {
                    required property var modelData
                    led: modelData.led
                    title: modelData.name
                    icon: modelData.led === 4 ? "sparkles" : "circle-dot"
                }
            }
        }
        Section {
            id: laptop
            visible: backend.laptopAvailable
            title: qsTr("Laptop keyboard")
            icon: "keyboard"
            note: qsTr("Through asusctl, on ASUS laptops such as the ROG Zephyrus G16.")
            readonly property var aura: backend.laptopAura
            function set(changes) {
                const a = Object.assign({}, laptop.aura, changes)
                backend.setLaptopAura(a.effect, a.color1, a.color2, a.speed, a.direction, a.brightness)
            }
            Segmented {
                options: [qsTr("Static"), qsTr("Breathing"), qsTr("Rainbow"), qsTr("Rainbow wave")]
                current: laptop.aura.effect
                onPicked: (i) => laptop.set({effect: i})
            }
            ColorRow {
                visible: laptop.aura.effect <= 1
                label: qsTr("Color")
                value: laptop.aura.color1
                onChosen: (c) => laptop.set({color1: c})
            }
            ColorRow {
                visible: laptop.aura.effect === 1
                label: qsTr("Second color")
                value: laptop.aura.color2
                onChosen: (c) => laptop.set({color2: c})
            }
            ColumnLayout {
                visible: laptop.aura.effect !== 0
                spacing: 8
                Label { text: qsTr("Speed") }
                Segmented {
                    options: [qsTr("Slow"), qsTr("Normal"), qsTr("Fast")]
                    current: laptop.aura.speed
                    onPicked: (i) => laptop.set({speed: i})
                }
            }
            ColumnLayout {
                visible: laptop.aura.effect === 3
                spacing: 8
                Label { text: qsTr("Direction") }
                Segmented {
                    options: ["↑", "↓", "←", "→"]
                    current: laptop.aura.direction
                    onPicked: (i) => laptop.set({direction: i})
                }
            }
            ColumnLayout {
                spacing: 8
                Label { text: qsTr("Brightness") }
                Segmented {
                    options: [qsTr("Off"), qsTr("Low"), qsTr("Medium"), qsTr("High")]
                    current: laptop.aura.brightness
                    onPicked: (i) => laptop.set({brightness: i})
                }
            }
        }
        Item { Layout.preferredHeight: 4 }
    }
}
