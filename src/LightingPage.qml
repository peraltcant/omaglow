import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ScrollView {
    id: page
    contentWidth: availableWidth
    readonly property var info: backend.effects[backend.effect]
    // Side by side when there's room, stacked otherwise.
    readonly property bool wide: availableWidth > 1050
    readonly property var swatches: ["#ffffff", "#ff0000", "#ff7a00", "#ffd60a", "#00ff00", "#00ffd0",
                                     "#00a2ff", "#0000ff", "#8a2bff", "#ff00ff", "#ff2d6f", "#ff9ec4"]
    // Clockwise from right, in 45 degree steps, as the keyboard counts them.
    readonly property var arrows: ["→", "↘", "↓", "↙", "←", "↖", "↑", "↗"]

    ColumnLayout {
        width: page.availableWidth
        spacing: 20

        RowLayout {
            Label { text: qsTr("Lighting"); font.pixelSize: 26; font.bold: true; Layout.fillWidth: true }
            Label { text: backend.lightingOn ? qsTr("On") : qsTr("Off"); opacity: 0.6 }
            Switch { checked: backend.lightingOn; onToggled: backend.lightingOn = checked }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: page.wide ? 2 : 1
            columnSpacing: 20
            rowSpacing: 20

            // Left: what it looks like, and which effect.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: 3
                Layout.alignment: Qt.AlignTop
                spacing: 20

                Section {
                    icon: "eye"
                    title: qsTr("Preview")
                    note: qsTr("An approximation of the effect; the keyboard draws the real one.")
                    KeyboardPreview {
                        Layout.fillWidth: true
                        // Never so tall that the effects fall off a short window.
                        Layout.preferredHeight: Math.min(implicitHeight, Math.max(150, page.height * 0.32))
                    }
                }

                Section {
                    icon: "sparkles"
                    title: qsTr("Effect")
                    GridLayout {
                        id: effectGrid
                        Layout.fillWidth: true
                        columns: width < 620 ? 3 : 5
                        columnSpacing: 8
                        rowSpacing: 8
                        Repeater {
                            model: backend.effects
                            Rectangle {
                                required property int index
                                required property var modelData
                                readonly property bool chosen: index === backend.effect && backend.lightingOn && !backend.customLighting
                                Layout.fillWidth: true
                                implicitHeight: 52
                                radius: Math.min(8, theme.radius)
                                color: chosen ? theme.accent : tap.hovered ? "#24242b" : "#1d1d22"
                                border.color: chosen ? theme.accent : "#2a2a31"
                                Label {
                                    anchors.centerIn: parent
                                    text: modelData.name
                                    font.pixelSize: 14
                                    font.bold: parent.chosen
                                    color: parent.chosen ? theme.accentForeground : "white"
                                }
                                HoverHandler { id: tap; cursorShape: Qt.PointingHandCursor }
                                TapHandler { onTapped: backend.effect = index }
                            }
                        }
                        // The design painted in Studio.
                        Rectangle {
                            readonly property bool chosen: backend.customLighting && backend.lightingOn
                            Layout.fillWidth: true
                            implicitHeight: 52
                            radius: Math.min(8, theme.radius)
                            color: chosen ? theme.accent : customHover.hovered ? "#24242b" : "#1d1d22"
                            border.color: chosen ? theme.accent : "#2a2a31"
                            Label {
                                anchors.centerIn: parent
                                text: qsTr("Custom")
                                font.pixelSize: 14
                                font.bold: parent.chosen
                                color: parent.chosen ? theme.accentForeground : "white"
                            }
                            HoverHandler { id: customHover; cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: backend.customLighting = true }
                        }
                    }
                    RowLayout {
                        visible: backend.customLighting && backend.lightingOn
                        Layout.fillWidth: true
                        Label {
                            text: qsTr("Custom shows the design and layers made in Studio.")
                            opacity: 0.6
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                        Button { text: qsTr("Edit in Studio"); icon.source: "qrc:/icons/pencil.svg"; flat: true; onClicked: win.page = 5 }
                    }
                }
            }

            // Right: how it behaves.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: 2
                Layout.alignment: Qt.AlignTop
                spacing: 20
                opacity: backend.lightingOn ? 1 : 0.4

                Section {
                    icon: "sliders-horizontal"
                    title: qsTr("Settings")
                    ColumnLayout {
                        spacing: 8
                        Layout.fillWidth: true
                        RowLayout {
                            Label { text: qsTr("Brightness"); Layout.fillWidth: true }
                            Label {
                                text: qsTr("%1%").arg(backend.brightness.toLocaleString(Qt.locale(), 'f', 0))
                                font.bold: true
                            }
                        }
                        Slider {
                            Layout.fillWidth: true
                            from: 0; to: 100; stepSize: 1
                            value: backend.brightness
                            onMoved: backend.brightness = value
                        }
                    }
                    ColumnLayout {
                        visible: page.info.speed && !backend.customLighting
                        spacing: 8
                        Label { text: qsTr("Speed") }
                        Segmented {
                            options: [qsTr("Fast"), qsTr("Normal"), qsTr("Slow")]
                            current: backend.speed
                            onPicked: (i) => backend.speed = i
                        }
                    }
                    ColumnLayout {
                        visible: page.info.directions.length > 0 && !backend.customLighting
                        spacing: 8
                        Label { text: qsTr("Direction") }
                        Segmented {
                            options: page.info.directions.map(d => page.arrows[d])
                            current: page.info.directions.indexOf(backend.direction)
                            onPicked: (i) => backend.direction = page.info.directions[i]
                        }
                    }
                    ColumnLayout {
                        visible: page.info.thickness && !backend.customLighting
                        spacing: 8
                        Label { text: qsTr("Thickness") }
                        Segmented {
                            options: [qsTr("Slim"), qsTr("Normal"), qsTr("Thick")]
                            current: backend.thickness
                            onPicked: (i) => backend.thickness = i
                        }
                    }
                }

                Section {
                    icon: "palette"
                    title: qsTr("Colors")
                    visible: (page.info.colors || page.info.random) && !backend.customLighting

                    // Static has a single color, so there is no mode to pick.
                    Segmented {
                        visible: page.info.random
                        options: [qsTr("Single"), qsTr("Double"), qsTr("Random")]
                        enabledOptions: [page.info.colors, page.info.colors, page.info.random]
                        current: backend.colorMode
                        onPicked: (i) => backend.colorMode = i
                    }

                    Repeater {
                        model: (page.info.colors && (backend.colorMode !== 2 || !page.info.random))
                               ? (backend.colorMode === 1 && page.info.random ? 2 : 1) : 0
                        ColumnLayout {
                            id: colorRow
                            required property int index
                            readonly property int which: index
                            readonly property color value: which === 0 ? backend.color1 : backend.color2
                            Layout.fillWidth: true
                            spacing: 10
                            RowLayout {
                                spacing: 12
                                Rectangle {
                                    width: 40; height: 40
                                    radius: Math.min(8, theme.radius)
                                    color: colorRow.value
                                    border.color: "#3a3a42"
                                }
                                Label {
                                    text: colorRow.which === 0 ? qsTr("Color") : qsTr("Second color")
                                    Layout.fillWidth: true
                                }
                                TextField {
                                    Layout.preferredWidth: 120
                                    placeholderText: qsTr("Hex")
                                    text: colorRow.value.toString()
                                    validator: RegularExpressionValidator { regularExpression: /#?[0-9a-fA-F]{6}/ }
                                    onEditingFinished: {
                                        var hex = text.charAt(0) === "#" ? text : "#" + text
                                        if (colorRow.which === 0) backend.color1 = hex; else backend.color2 = hex
                                    }
                                }
                            }
                            GridLayout {
                                Layout.fillWidth: true
                                columns: 6
                                columnSpacing: 8
                                rowSpacing: 8
                                Repeater {
                                    model: page.swatches
                                    Rectangle {
                                        required property string modelData
                                        readonly property bool chosen: colorRow.value.toString() === Qt.color(modelData).toString()
                                        Layout.fillWidth: true
                                        implicitHeight: 34
                                        radius: Math.min(6, theme.radius)
                                        color: modelData
                                        border.width: chosen ? 3 : 1
                                        border.color: chosen ? theme.accent : "#33333a"
                                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                                        TapHandler {
                                            onTapped: colorRow.which === 0 ? backend.color1 = parent.modelData
                                                                           : backend.color2 = parent.modelData
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        LooksSection { Layout.fillWidth: true }
        Item { Layout.preferredHeight: 4 }
    }
}
