import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ScrollView {
    id: page
    contentWidth: availableWidth
    readonly property bool wide: availableWidth > 1050

    // The display shows one thing at a time: a picture, a built-in preset,
    // music, or the clock. "live" is what the keyboard shows now; "view" is
    // the mode whose options are open, which starts on the live one.
    readonly property int live: backend.oledMusic ? 2 : backend.oledClock ? 3
                              : backend.oledFrames > 0 && backend.oledFile !== "" ? 0
                              : backend.oledPreset >= 0 ? 1 : -1
    property int view: live >= 0 ? live : 1
    onLiveChanged: if (live >= 0) view = live
    readonly property var modes: [qsTr("Picture"), qsTr("Preset"), qsTr("Music"), qsTr("Clock")]

    ColumnLayout {
        width: page.availableWidth
        spacing: 20

        RowLayout {
            Label { text: qsTr("OLED display"); font.pixelSize: 26; font.bold: true; Layout.fillWidth: true }
            Label { text: backend.oledOn ? qsTr("On") : qsTr("Off"); opacity: 0.6 }
            Switch { checked: backend.oledOn; onToggled: backend.oledOn = checked }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: page.wide ? 2 : 1
            columnSpacing: 20
            rowSpacing: 20

            // In two columns: preview and brightness left, choices right.
            // Stacked: preview, then the choices, then brightness.
            Section {
                Layout.row: 0
                Layout.column: 0
                Layout.alignment: Qt.AlignTop
                Layout.preferredWidth: 3
                opacity: backend.oledOn ? 1 : 0.4
                icon: "monitor"
                title: page.live >= 0 ? qsTr("On the keyboard · %1").arg(page.modes[page.live]) : qsTr("On the keyboard")

                // 208x64, black like the panel, scaled to fit.
                Rectangle {
                    id: screen
                    Layout.fillWidth: true
                    Layout.preferredHeight: width * 64 / 208
                    color: "black"
                    radius: Math.min(8, theme.radius)
                    border.color: "#2a2a2e"
                    readonly property real innerWidth: width - 16
                    readonly property real innerHeight: innerWidth * 64 / 208

                    Image {
                        id: picture
                        anchors.centerIn: parent
                        width: screen.innerWidth; height: screen.innerHeight
                        smooth: false
                        cache: false
                        visible: page.live === 0
                        property int frame: 0
                        source: visible ? "image://oled/" + frame + "?" + backend.previewRevision : ""
                        Timer {
                            running: picture.visible && backend.oledFrames > 1
                            repeat: true
                            interval: 100
                            onTriggered: picture.frame = (picture.frame + 1) % backend.oledFrames
                        }
                    }
                    // The keyboard draws its built-in animations itself; ASUS's
                    // artwork isn't shipped, so the preview names the one chosen.
                    Column {
                        anchors.centerIn: parent
                        visible: page.live === 1
                        spacing: 6
                        Icon {
                            anchors.horizontalCenter: parent.horizontalCenter
                            glyph: "sparkles"
                            size: 28
                            opacity: 0.8
                        }
                        Label {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: qsTr("Built-in animation %1").arg((backend.oledPreset + 1).toLocaleString(Qt.locale(), 'f', 0))
                            opacity: 0.8
                        }
                    }
                    // Music: bars that move like the keyboard's do.
                    Item {
                        id: bars
                        visible: page.live === 2
                        anchors.centerIn: parent
                        width: screen.innerWidth
                        height: screen.innerHeight * 0.85
                        property real t: 0
                        Timer {
                            running: bars.visible
                            repeat: true
                            interval: 100
                            onTriggered: bars.t += 0.1
                        }
                        Repeater {
                            model: 26
                            Rectangle {
                                required property int index
                                readonly property real level: 0.15 + 0.85 * Math.abs(Math.sin(bars.t * (1.3 + index * 0.17) + index))
                                                               * Math.max(0.25, 1 - index / 30)
                                x: index * bars.width / 26 + bars.width / 26 * 0.125
                                width: bars.width / 26 * 0.75
                                height: bars.height * level
                                y: backend.musicStyle === 2 ? (bars.height - height) / 2 : bars.height - height
                                color: "white"
                                opacity: 0.85
                            }
                        }
                    }
                    Label {
                        anchors.centerIn: parent
                        visible: page.live === 3
                        text: Qt.formatTime(clockNow.now, "HH:mm")
                        font.pixelSize: screen.innerHeight * 0.55
                        font.bold: true
                        Timer {
                            id: clockNow
                            property date now: new Date()
                            interval: 1000
                            repeat: true
                            running: page.live === 3
                            onTriggered: now = new Date()
                        }
                    }
                    Label {
                        anchors.centerIn: parent
                        visible: page.live < 0
                        opacity: 0.5
                        text: qsTr("Pick what the display shows")
                    }
                }
                ProgressBar {
                    Layout.fillWidth: true
                    visible: backend.busy
                    value: backend.progress
                }
            }

            Section {
                Layout.row: page.wide ? 1 : 2
                Layout.column: 0
                Layout.alignment: Qt.AlignTop
                Layout.preferredWidth: 3
                opacity: backend.oledOn ? 1 : 0.4
                icon: "sun"
                title: qsTr("Brightness")
                RowLayout {
                    spacing: 16
                    Slider {
                        Layout.fillWidth: true
                        from: 0; to: 100; stepSize: 1
                        value: backend.oledBrightness
                        // Each change is a command to the keyboard, so send once on release.
                        onPressedChanged: if (!pressed) backend.oledBrightness = value
                    }
                    Label {
                        text: qsTr("%1%").arg(backend.oledBrightness.toLocaleString(Qt.locale(), 'f', 0))
                        font.bold: true
                        Layout.preferredWidth: 48
                        horizontalAlignment: Text.AlignRight
                    }
                }
            }

            // Right: choose what to show.
            ColumnLayout {
                Layout.row: page.wide ? 0 : 1
                Layout.column: page.wide ? 1 : 0
                Layout.rowSpan: page.wide ? 2 : 1
                Layout.fillWidth: true
                Layout.preferredWidth: 2
                Layout.alignment: Qt.AlignTop
                spacing: 20
                opacity: backend.oledOn ? 1 : 0.4

                Section {
                    icon: "eye"
                    title: qsTr("Show")
                    note: page.live >= 0 ? qsTr("● marks what the keyboard shows now.") : ""
                    Segmented {
                        Layout.fillWidth: true
                        options: page.modes.map((m, i) => i === page.live ? "● " + m : m)
                        current: page.view
                        onPicked: (i) => page.view = i
                    }
                }

                // Picture
                Section {
                    visible: page.view === 0
                    icon: "image"
                    title: qsTr("Picture")
                    note: qsTr("PNG, JPEG, BMP or an animated GIF (up to 196 frames), fitted to 208×64 in 16 grays.")
                    Button {
                        text: qsTr("Open picture or GIF…")
                        icon.source: "qrc:/icons/image.svg"
                        highlighted: true
                        onClicked: backend.openOledImage()
                    }
                    Label {
                        visible: backend.oledFile !== ""
                        Layout.fillWidth: true
                        opacity: 0.7
                        elide: Text.ElideMiddle
                        text: qsTr("%n frames", "", backend.oledFrames) + " · " + backend.oledFile.split("/").pop()
                    }
                }

                // Preset
                Section {
                    visible: page.view === 1
                    icon: "sparkles"
                    title: qsTr("Built-in animations")
                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: 10
                        rowSpacing: 10
                        Repeater {
                            model: 6
                            Rectangle {
                                id: tile
                                required property int index
                                readonly property bool chosen: index === backend.oledPreset && page.live === 1
                                Layout.fillWidth: true
                                Layout.preferredHeight: width * 64 / 208 + 30
                                radius: Math.min(8, theme.radius)
                                color: "black"
                                border.width: chosen ? 2 : 1
                                border.color: chosen ? theme.accent : hover.hovered ? "#4a4a52" : "#2a2a31"
                                Icon {
                                    anchors.centerIn: parent
                                    anchors.verticalCenterOffset: -8
                                    glyph: "sparkles"
                                    size: 22
                                    color: tile.chosen ? theme.accent : "white"
                                    opacity: tile.chosen ? 1 : 0.6
                                }
                                Label {
                                    anchors.bottom: parent.bottom
                                    anchors.bottomMargin: 6
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: (tile.index + 1).toLocaleString(Qt.locale(), 'f', 0)
                                    font.pixelSize: 13
                                    font.bold: tile.chosen
                                    color: tile.chosen ? theme.accent : "white"
                                    opacity: tile.chosen ? 1 : 0.6
                                }
                                HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                                TapHandler { onTapped: backend.chooseOledPreset(tile.index) }
                            }
                        }
                    }
                }

                // Music
                Section {
                    visible: page.view === 2
                    icon: "music"
                    title: qsTr("Music")
                    note: qsTr("Shows what is playing as spectrum bars. A small background service listens to your default audio output while music is shown.")
                    ColumnLayout {
                        spacing: 8
                        Label { text: qsTr("Style") }
                        Segmented {
                            options: [qsTr("Bars"), qsTr("Mirrored"), qsTr("Line")]
                            current: backend.musicStyle - 1
                            onPicked: (i) => backend.musicStyle = i + 1
                        }
                    }
                    Button {
                        text: backend.oledMusic ? qsTr("Stop music") : qsTr("Show music on the keyboard")
                        icon.source: backend.oledMusic ? "qrc:/icons/square.svg" : "qrc:/icons/music.svg"
                        highlighted: !backend.oledMusic
                        onClicked: backend.oledMusic = !backend.oledMusic
                    }
                }

                // Clock
                Section {
                    visible: page.view === 3
                    icon: "clock"
                    title: qsTr("Clock")
                    note: qsTr("The keyboard has no clock of its own, so a small background service sends it the time every minute while the clock is shown.")
                    Button {
                        text: backend.oledClock ? qsTr("Stop the clock") : qsTr("Show the clock on the keyboard")
                        icon.source: backend.oledClock ? "qrc:/icons/square.svg" : "qrc:/icons/clock.svg"
                        highlighted: !backend.oledClock
                        onClicked: backend.oledClock = !backend.oledClock
                    }
                }
            }
        }
        Item { Layout.preferredHeight: 4 }
    }
}
