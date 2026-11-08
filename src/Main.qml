import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 1040
    height: 700
    minimumWidth: 640
    minimumHeight: 560
    visible: true
    title: "Omaglow"
    color: "#0e0e10"

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius
    font.pixelSize: 15

    property bool helpVisible: false
    // Narrow windows (a tiled half or third of the screen) get a slim
    // sidebar with just the section numbers.
    readonly property bool compact: width < 1000
    property int page: 0
    readonly property var info: backend.effects[backend.effect]
    readonly property bool connected: backend.connection === 2
    readonly property var pages: {
        translator.language
        return [qsTr("Lighting"), qsTr("OLED"), qsTr("Keys"), qsTr("Knob"), qsTr("Power"), qsTr("Studio"), qsTr("Devices")]
    }
    readonly property var pageIcons: ["sun", "monitor", "keyboard", "circle-dot", "battery", "palette", "mouse"]

    onClosing: backend.flush()

    function choiceIndex(code, param) {
        var choices = backend.keyChoices
        for (var i = 0; i < choices.length; ++i)
            if (choices[i].code === code && choices[i].param === param) return i
        return 0
    }

    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Esc"; context: Qt.ApplicationShortcut; enabled: win.helpVisible; onActivated: win.helpVisible = false }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; onActivated: { backend.flush(); Qt.quit() } }
    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; enabled: win.page === 0
               onActivated: backend.lightingOn = !backend.lightingOn }
    Shortcut { sequence: "1"; context: Qt.ApplicationShortcut; onActivated: win.page = 0 }
    Shortcut { sequence: "2"; context: Qt.ApplicationShortcut; onActivated: win.page = 1 }
    Shortcut { sequence: "3"; context: Qt.ApplicationShortcut; onActivated: win.page = 2 }
    Shortcut { sequence: "4"; context: Qt.ApplicationShortcut; onActivated: win.page = 3 }
    Shortcut { sequence: "5"; context: Qt.ApplicationShortcut; onActivated: win.page = 4 }
    Shortcut { sequence: "6"; context: Qt.ApplicationShortcut; onActivated: win.page = 5 }
    Shortcut { sequence: "7"; context: Qt.ApplicationShortcut; onActivated: win.page = 6 }
    Shortcut { sequence: "Up"; context: Qt.ApplicationShortcut; enabled: win.page === 0
               onActivated: backend.effect = (backend.effect + 9) % 10 }
    Shortcut { sequence: "Down"; context: Qt.ApplicationShortcut; enabled: win.page === 0
               onActivated: backend.effect = (backend.effect + 1) % 10 }
    Shortcut { sequence: "Left"; context: Qt.ApplicationShortcut; enabled: win.page < 2
               onActivated: win.page === 0 ? backend.brightness = Math.max(0, backend.brightness - 10)
                                           : backend.oledBrightness = Math.max(0, backend.oledBrightness - 10) }
    Shortcut { sequence: "Right"; context: Qt.ApplicationShortcut; enabled: win.page < 2
               onActivated: win.page === 0 ? backend.brightness = Math.min(100, backend.brightness + 10)
                                           : backend.oledBrightness = Math.min(100, backend.oledBrightness + 10) }
    Shortcut { sequence: "S"; context: Qt.ApplicationShortcut; enabled: win.page === 0 && win.info.speed
               onActivated: backend.speed = (backend.speed + 1) % 3 }
    Shortcut { sequence: "O"; context: Qt.ApplicationShortcut; onActivated: { win.page = 1; backend.openOledImage() } }
    Shortcut { sequence: "R"; context: Qt.ApplicationShortcut; onActivated: backend.refresh() }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        RowLayout {
            anchors.fill: parent
            spacing: 0

            // Sections on the left, the keyboard's state at the bottom.
            Rectangle {
                Layout.fillHeight: true
                Layout.preferredWidth: win.compact ? 72 : 240
                color: "#131316"

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: win.compact ? 10 : 14
                    spacing: 4

                    Label {
                        text: win.compact ? "Om" : "Omaglow"
                        font.pixelSize: 20
                        font.bold: true
                        Layout.alignment: win.compact ? Qt.AlignHCenter : Qt.AlignLeft
                        Layout.leftMargin: win.compact ? 0 : 8
                        Layout.topMargin: 6
                        Layout.bottomMargin: 10
                    }

                    // Each profile keeps its own lighting, keys and knob.
                    ComboBox {
                        id: profileBox
                        Layout.fillWidth: true
                        Layout.bottomMargin: 12
                        enabled: win.connected && !backend.busy
                        // The keyboard numbers the default profile 6.
                        model: {
                            translator.language
                            return win.compact ? ["1", "2", "3", "4", "5", "D"]
                                               : [qsTr("Profile %1").arg(1), qsTr("Profile %1").arg(2), qsTr("Profile %1").arg(3),
                                                  qsTr("Profile %1").arg(4), qsTr("Profile %1").arg(5), qsTr("Default profile")]
                        }
                        currentIndex: backend.profile - 1
                        // Swapping the labels (slim sidebar) resets the index; put it back.
                        onModelChanged: currentIndex = backend.profile - 1
                        Connections {
                            target: backend
                            function onProfileChanged() { profileBox.currentIndex = backend.profile - 1 }
                        }
                        onActivated: (i) => backend.profile = i + 1
                        // No room for the arrow in the slim sidebar; the number says enough.
                        Component.onCompleted: if (indicator) indicator.visible = Qt.binding(() => !win.compact)
                        leftPadding: win.compact ? 0 : 12
                        rightPadding: win.compact ? 0 : 40
                        contentItem: Label {
                            text: profileBox.displayText
                            horizontalAlignment: win.compact ? Text.AlignHCenter : Text.AlignLeft
                            verticalAlignment: Text.AlignVCenter
                            leftPadding: win.compact ? 0 : 4
                            elide: Text.ElideRight
                        }
                        ToolTip.visible: hovered
                        ToolTip.delay: 600
                        ToolTip.text: qsTr("Lighting, keys and knob are kept per profile. Fn + 1–6 on the keyboard switches profiles too; pick the same one here to match.")
                    }

                    Repeater {
                        model: win.pages
                        Rectangle {
                            id: navItem
                            required property int index
                            required property string modelData
                            readonly property bool current: index === win.page
                            Layout.fillWidth: true
                            implicitHeight: 46
                            radius: Math.min(6, theme.radius)
                            color: current ? "#22222a" : hover.hovered ? "#1b1b20" : "transparent"
                            Rectangle {
                                visible: navItem.current
                                width: 3; height: 20
                                radius: 1
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left: parent.left
                                color: theme.accent
                            }
                            Icon {
                                id: navIcon
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left: win.compact ? undefined : parent.left
                                anchors.horizontalCenter: win.compact ? parent.horizontalCenter : undefined
                                anchors.leftMargin: 16
                                glyph: win.pageIcons[navItem.index]
                                size: win.compact ? 20 : 18
                                color: navItem.current ? theme.accent : "white"
                                opacity: navItem.current ? 1 : 0.8
                            }
                            Label {
                                visible: !win.compact
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left: navIcon.right
                                anchors.leftMargin: 12
                                text: navItem.modelData
                                font.pixelSize: 15
                                color: navItem.current ? theme.accent : "white"
                            }
                            // The keyboard shortcut for the page.
                            Label {
                                visible: !win.compact
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.right: parent.right
                                anchors.rightMargin: 12
                                text: (navItem.index + 1).toLocaleString(Qt.locale(), 'f', 0)
                                opacity: 0.3
                                font.pixelSize: 12
                            }
                            ToolTip.visible: win.compact && hover.hovered
                            ToolTip.text: navItem.modelData
                            HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: win.page = navItem.index }
                        }
                    }

                    Item { Layout.fillHeight: true }

                    // Connection state.
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: stateColumn.implicitHeight + 24
                        radius: Math.min(8, theme.radius)
                        color: "#17171b"
                        border.color: "#232329"
                        ColumnLayout {
                            id: stateColumn
                            anchors.fill: parent
                            anchors.margins: 12
                            spacing: 6
                            RowLayout {
                                spacing: 8
                                Icon {
                                    size: 16
                                    glyph: win.connected && backend.asleep ? "moon"
                                        : win.connected ? (backend.wireless ? "wifi" : "plug") : "circle-alert"
                                    color: win.connected && !backend.asleep ? "#3ddc84"
                                        : (win.connected || backend.connection === 1 || backend.connection === 3) ? "#ffb020" : "#ff5555"
                                }
                                Label {
                                    visible: !win.compact
                                    text: win.connected && backend.asleep ? qsTr("Asleep · press a key")
                                        : win.connected ? (backend.wireless ? qsTr("Connected · 2.4 GHz") : qsTr("Connected · USB"))
                                        : backend.connection === 1 ? qsTr("No permission")
                                        : backend.connection === 3 ? qsTr("Not responding") : qsTr("Not found")
                                    font.pixelSize: 13
                                }
                            }
                            Label {
                                visible: !win.compact && win.connected && backend.firmware !== ""
                                text: qsTr("Firmware %1").arg(backend.firmware)
                                opacity: 0.6
                                font.pixelSize: 12
                            }
                            RowLayout {
                                visible: win.connected && backend.battery >= 0
                                spacing: 8
                                Icon {
                                    visible: !win.compact
                                    size: 16
                                    glyph: backend.battery <= 20 ? "battery-low" : "battery"
                                    color: backend.battery <= 20 ? "#ff5555" : "white"
                                    opacity: backend.battery <= 20 ? 1 : 0.6
                                }
                                Rectangle {
                                    visible: !win.compact
                                    Layout.preferredWidth: 80
                                    implicitHeight: 6
                                    radius: 3
                                    color: "#2a2a31"
                                    Rectangle {
                                        width: parent.width * Math.max(0, backend.battery) / 100
                                        height: parent.height
                                        radius: 3
                                        color: backend.battery <= 20 ? "#ff5555" : theme.accent
                                    }
                                }
                                Label {
                                    text: qsTr("%1%").arg(backend.battery.toLocaleString(Qt.locale(), 'f', 0))
                                    opacity: 0.6
                                    font.pixelSize: 12
                                }
                            }
                        }
                    }
                    Label {
                        visible: !win.compact
                        Layout.topMargin: 8
                        Layout.leftMargin: 8
                        opacity: 0.35
                        font.pixelSize: 12
                        text: qsTr("Press ? for shortcuts")
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: win.compact ? 18 : 32
                spacing: 16

                // Says what's wrong instead of quietly doing less.
                Rectangle {
                    Layout.fillWidth: true
                    visible: !win.connected || backend.error !== ""
                    implicitHeight: notice.implicitHeight + 24
                    radius: Math.min(8, theme.radius)
                    color: "#2a1f12"
                    border.color: "#ffb020"
                    RowLayout {
                        id: notice
                        anchors.fill: parent
                        anchors.margins: 12
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: backend.error !== "" ? backend.error
                                : backend.connection === 1
                                  ? qsTr("Can't open %1. Add a udev rule so your user can reach the keyboard:\nKERNEL==\"hidraw*\", ATTRS{idVendor}==\"0b05\", ATTRS{idProduct}==\"1a83\", TAG+=\"uaccess\"\nin /etc/udev/rules.d/70-omaglow-rog-azoth.rules, then unplug and replug the keyboard.").arg(backend.devicePath)
                                  : backend.connection === 3
                                  ? qsTr("The keyboard is plugged in but doesn't answer. Unplug it and plug it back in, or turn it off and on.")
                                  : qsTr("Connect the ROG Azoth with its cable or 2.4 GHz dongle. A virtual machine holding the keyboard also hides it.")
                        }
                        Button {
                            text: backend.error !== "" ? qsTr("Dismiss") : qsTr("Retry")
                            icon.source: backend.error !== "" ? "qrc:/icons/x.svg" : "qrc:/icons/refresh-cw.svg"
                            flat: true
                            onClicked: backend.error !== "" ? backend.clearError() : backend.refresh()
                        }
                    }
                }

                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.maximumWidth: 1500
                    currentIndex: win.page
                    // Devices works without the keyboard.
                    enabled: (win.connected || win.page === 6) && !backend.busy

                    LightingPage {}
                    OledPage {}
                    KeysPage {}
                    KnobPage {}
                    PowerPage {}
                    StudioPage {}
                    DevicesPage {}
                }
            }
        }

        // Every shortcut, toggled with ?.
        Rectangle {
            anchors.fill: parent
            visible: win.helpVisible
            color: "#e00e0e10"
            TapHandler { onTapped: win.helpVisible = false }
            Rectangle {
                anchors.centerIn: parent
                width: grid.implicitWidth + 56
                height: grid.implicitHeight + 56
                radius: Math.min(12, theme.radius)
                color: "#17171b"
                border.color: "#2a2a31"
                GridLayout {
                    id: grid
                    anchors.centerIn: parent
                    columns: 2
                    columnSpacing: 32
                    rowSpacing: 10
                    Repeater {
                        model: [
                            ["1 – 7", qsTr("Lighting, OLED, Keys, Knob, Power, Studio, Devices")],
                            ["Space", qsTr("Lighting on or off")],
                            ["↑ ↓", qsTr("Previous or next effect")],
                            ["← →", qsTr("Brightness down or up")],
                            ["S", qsTr("Change speed")],
                            ["O", qsTr("Open a picture for the display")],
                            ["R", qsTr("Look for the keyboard again")],
                            ["?", qsTr("Show or hide shortcuts")],
                            ["Q", qsTr("Quit")]
                        ]
                        delegate: RowLayout {
                            required property var modelData
                            Layout.columnSpan: 2
                            spacing: 24
                            Label { text: modelData[0]; font.bold: true; color: theme.accent; Layout.preferredWidth: 70 }
                            Label { text: modelData[1] }
                        }
                    }
                }
            }
        }
    }
}
