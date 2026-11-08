import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Paint the keys one by one, and put groups of them in layers with an effect
// each, as in Aura Creator; the keyboard shows it in direct mode.
ScrollView {
    id: page
    contentWidth: availableWidth
    property color brush: "#ff0000"
    // -1 paints the design; otherwise the layer clicked keys go into.
    property int layerIndex: -1
    property bool takeOut: false
    readonly property var current: layerIndex >= 0 && layerIndex < backend.layers.length ? backend.layers[layerIndex] : null
    // Effects that use the colors, and the ones where random colors make sense.
    // By effect: Static, Breathing, Strobing, Color cycle, Rainbow, Starry night,
    // Comet, Tide, Current, Fire, Plasma, Scanner, Fireflies, Heartbeat, Gradient.
    readonly property var usesColors: [true, true, true, false, false, true, true, true, true,
                                       true, true, true, true, true, true]
    readonly property var usesSecond: [false, true, true, false, false, true, true, true, true,
                                       true, true, true, true, true, true]
    readonly property var usesRandom: [false, true, true, false, false, true, true, false, true,
                                       false, true, false, true, false, false]
    // Still effects have no speed.
    readonly property var usesSpeed: [false, true, true, true, true, true, true, true, true,
                                      true, true, true, true, true, false]
    // 0 one color, 1 two colors, 2 random; one color is the same color twice.
    readonly property int colorMode: !current ? -1
        : current.randomColors && usesRandom[current.effect] ? 2
        : Qt.colorEqual(current.color1, current.color2) ? 0 : 1
    function pickColorMode(mode) {
        backend.setLayerRandom(layerIndex, mode === 2)
        if (mode === 0)
            backend.setLayerColor(layerIndex, 1, current.color1)
        else if (mode === 1 && Qt.colorEqual(current.color1, current.color2))
            backend.setLayerColor(layerIndex, 1, Qt.colorEqual(current.color1, "#00a2ff") ? "#ff2d6f" : "#00a2ff")
    }
    property var frame: ({})
    readonly property var stillFrame: { backend.design; backend.layers; return backend.frameAt(0) }
    // Painting the design shows the design itself, even under layers.
    readonly property var shown: current === null ? backend.design : backend.animatedLighting ? frame : stillFrame
    // Keys a layer draws over, marked while painting the design.
    readonly property var covered: {
        var set = {}
        for (var i = 0; i < backend.layers.length; ++i)
            for (var j = 0; j < backend.layers[i].keys.length; ++j)
                set[backend.layers[i].keys[j]] = true
        return set
    }
    readonly property bool wide: availableWidth > 1050

    // Another profile or look may have fewer layers: back to the design.
    Connections {
        target: backend
        function onProfileChanged() { page.layerIndex = -1 }
        function onLightingChanged() { if (page.layerIndex >= backend.layers.length) page.layerIndex = -1 }
    }

    Timer {
        interval: 62
        repeat: true
        running: page.visible && backend.animatedLighting
        onTriggered: page.frame = backend.frameAt(Date.now())
    }

    readonly property var swatches: ["#ffffff", "#ff0000", "#ff7a00", "#ffd60a", "#00ff00", "#00ffd0",
                                     "#00a2ff", "#0000ff", "#8a2bff", "#ff00ff", "#ff2d6f", "#000000"]

    ColumnLayout {
        width: Math.min(page.availableWidth, 1250)
        spacing: 16

        RowLayout {
            Label { text: qsTr("Studio"); font.pixelSize: 26; font.bold: true; Layout.fillWidth: true }
            Label { text: backend.customLighting && backend.lightingOn ? qsTr("Showing on the keyboard") : qsTr("Not in use"); opacity: 0.6 }
            Switch {
                checked: backend.customLighting && backend.lightingOn
                onToggled: backend.customLighting = checked
            }
        }

        Section {
            icon: "layers"
            title: qsTr("Layers")
            note: qsTr("The design is the bottom layer. Each layer is a group of keys with its own effect, and covers the layers to its left.")
            Segmented {
                options: [qsTr("Design")].concat(backend.layers.map(l => l.name))
                current: page.layerIndex + 1
                onPicked: (i) => page.layerIndex = i - 1
            }
            Flow {
                Layout.fillWidth: true
                spacing: 8
                Button { text: qsTr("Add layer"); icon.source: "qrc:/icons/plus.svg"; onClicked: page.layerIndex = backend.addLayer("") }
                Button {
                    text: qsTr("Move left"); icon.source: "qrc:/icons/arrow-left.svg"
                    flat: true
                    enabled: page.layerIndex > 0
                    onClicked: { backend.moveLayer(page.layerIndex, page.layerIndex - 1); page.layerIndex -= 1 }
                }
                Button {
                    text: qsTr("Move right"); icon.source: "qrc:/icons/arrow-right.svg"
                    flat: true
                    enabled: page.layerIndex >= 0 && page.layerIndex < backend.layers.length - 1
                    onClicked: { backend.moveLayer(page.layerIndex, page.layerIndex + 1); page.layerIndex += 1 }
                }
                Button {
                    text: qsTr("Delete layer"); icon.source: "qrc:/icons/trash-2.svg"
                    flat: true
                    enabled: page.current !== null
                    onClicked: deleteLayerDialog.open()
                }
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: page.wide ? 2 : 1
            columnSpacing: 20
            rowSpacing: 16

            Section {
                Layout.fillWidth: true
                Layout.preferredWidth: 3
                Layout.alignment: Qt.AlignTop
                icon: "keyboard"
                title: page.current ? qsTr("Keys in %1").arg(page.current.name) : qsTr("Paint the keys")
                note: page.current ? (page.current.keys.length === 0
                                      ? qsTr("No keys yet. Click or drag across the keys, or pick All keys, to add them to the layer.")
                                      : qsTr("Click or drag across the keys to add them to the layer."))
                                   : Object.keys(page.covered).length > 0
                                     ? qsTr("Click or drag across the keys. Keys with a dot are covered by a layer, which draws over them.")
                                     : qsTr("Click or drag across the keys. Each profile keeps its own design; brightness is set in Lighting.")
                RowLayout {
                    visible: page.current !== null
                    Label { text: qsTr("Take keys out instead"); Layout.fillWidth: true }
                    Switch { checked: page.takeOut; onToggled: page.takeOut = checked }
                }
                Flow {
                    visible: page.current !== null
                    Layout.fillWidth: true
                    spacing: 8
                    Button {
                        text: qsTr("All keys"); icon.source: "qrc:/icons/square-check.svg"
                        flat: true
                        onClicked: backend.setLayerKeys(page.layerIndex, backend.keys.map(k => k.code), true)
                    }
                    Button {
                        text: qsTr("Keys in no layer"); icon.source: "qrc:/icons/square-dashed.svg"
                        flat: true
                        onClicked: backend.setLayerKeys(page.layerIndex,
                                                        backend.keys.map(k => k.code).filter(c => !page.covered[c]), true)
                    }
                    Button {
                        text: qsTr("Take all out"); icon.source: "qrc:/icons/x.svg"
                        flat: true
                        enabled: page.current !== null && page.current.keys.length > 0
                        onClicked: backend.setLayerKeys(page.layerIndex, page.current.keys, false)
                    }
                }

                Item {
                    id: board
                    Layout.fillWidth: true
                    readonly property real unit: width / 16.25
                    Layout.preferredHeight: unit * 6.25

                    function keyAt(px, py) {
                        var keys = backend.keys
                        for (var i = 0; i < keys.length; ++i) {
                            var k = keys[i]
                            if (px >= k.x * unit && px < (k.x + k.width) * unit && py >= k.y * unit && py < (k.y + 1) * unit)
                                return k.code
                        }
                        return -1
                    }
                    property int lastPainted: -1
                    function paintAt(px, py) {
                        var code = keyAt(px, py)
                        if (code < 0 || code === lastPainted) return
                        lastPainted = code
                        if (page.current)
                            backend.setLayerKeys(page.layerIndex, [code], !page.takeOut)
                        else
                            backend.paintKeys([code], page.brush)
                    }

                    Repeater {
                        model: backend.keys
                        Rectangle {
                            required property var modelData
                            readonly property var painted: page.shown[String(modelData.code)]
                            readonly property bool inLayer: page.current !== null && page.current.keys.indexOf(modelData.code) >= 0
                            x: modelData.x * board.unit + 2
                            y: modelData.y * board.unit + 2
                            width: modelData.width * board.unit - 4
                            height: board.unit - 4
                            radius: Math.min(4, theme.radius)
                            color: painted ? painted : "#141418"
                            border.width: inLayer ? 3 : 1
                            border.color: inLayer ? theme.accent : "#2a2a31"
                            // Outlined so it reads on any color, and doesn't flip as frames change.
                            Label {
                                anchors.centerIn: parent
                                text: modelData.label
                                font.pixelSize: Math.max(9, board.unit * 0.24)
                                color: "white"
                                style: Text.Outline
                                styleColor: "black"
                            }
                            Rectangle {
                                visible: page.current === null && page.covered[modelData.code] === true
                                anchors.top: parent.top
                                anchors.right: parent.right
                                anchors.margins: 4
                                width: Math.max(5, board.unit * 0.12); height: width
                                radius: width / 2
                                color: theme.accent
                                border.color: "black"
                            }
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onPressed: (mouse) => { board.lastPainted = -1; board.paintAt(mouse.x, mouse.y) }
                        onPositionChanged: (mouse) => { if (pressed) board.paintAt(mouse.x, mouse.y) }
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: 2
                Layout.alignment: Qt.AlignTop
                spacing: 16

                Section {
                    Layout.fillWidth: true
                    icon: "sparkles"
                    title: page.current ? qsTr("%1 effect").arg(page.current.name) : ""
                    visible: page.current !== null
                    RowLayout {
                        Label { text: qsTr("Name"); Layout.preferredWidth: 110 }
                        TextField {
                            Layout.preferredWidth: 220
                            text: page.current ? page.current.name : ""
                            onEditingFinished: backend.renameLayer(page.layerIndex, text)
                        }
                    }
                    Segmented {
                        options: backend.layerEffects
                        current: page.current ? page.current.effect : -1
                        onPicked: (i) => backend.setLayerEffect(page.layerIndex, i)
                    }
                    ColumnLayout {
                        visible: page.current !== null && page.usesSpeed[page.current.effect]
                        spacing: 8
                        Label { text: qsTr("Speed") }
                        Segmented {
                            options: [qsTr("Fast"), qsTr("Normal"), qsTr("Slow")]
                            current: page.current ? page.current.speed : -1
                            onPicked: (i) => backend.setLayerSpeed(page.layerIndex, i)
                        }
                    }
                    Segmented {
                        visible: page.current !== null && page.usesSecond[page.current.effect]
                        options: [qsTr("One color"), qsTr("Two colors"), qsTr("Random")]
                        enabledOptions: page.current ? [true, true, page.usesRandom[page.current.effect]] : []
                        current: page.colorMode
                        onPicked: (i) => page.pickColorMode(i)
                    }
                    ColorRow {
                        visible: page.current !== null && page.usesColors[page.current.effect]
                                 && !(page.current.randomColors && page.usesRandom[page.current.effect])
                        label: qsTr("Color")
                        value: page.current ? page.current.color1 : "white"
                        onChosen: (c) => {
                            // Read first: the mode follows the colors as soon as one changes.
                            const one = page.colorMode === 0
                            backend.setLayerColor(page.layerIndex, 0, c)
                            if (one)
                                backend.setLayerColor(page.layerIndex, 1, c)
                        }
                    }
                    ColorRow {
                        visible: page.current !== null && page.usesSecond[page.current.effect] && page.colorMode === 1
                        label: qsTr("Second color")
                        value: page.current ? page.current.color2 : "blue"
                        onChosen: (c) => backend.setLayerColor(page.layerIndex, 1, c)
                    }
                }

                Section {
                    Layout.fillWidth: true
                    icon: "brush"
                    title: qsTr("Brush")
                    visible: page.current === null
                    RowLayout {
                        spacing: 12
                        Rectangle {
                            width: 40; height: 40
                            radius: Math.min(8, theme.radius)
                            color: page.brush
                            border.color: "#3a3a42"
                        }
                        TextField {
                            Layout.preferredWidth: 120
                            placeholderText: qsTr("Hex")
                            text: page.brush.toString()
                            validator: RegularExpressionValidator { regularExpression: /#?[0-9a-fA-F]{6}/ }
                            onEditingFinished: page.brush = text.charAt(0) === "#" ? text : "#" + text
                        }
                        Item { Layout.fillWidth: true }
                    }
                    GridLayout {
                        Layout.fillWidth: true
                        columns: 12
                        columnSpacing: 8
                        rowSpacing: 8
                        Repeater {
                            model: page.swatches
                            Rectangle {
                                required property string modelData
                                readonly property bool chosen: page.brush.toString() === Qt.color(modelData).toString()
                                Layout.fillWidth: true
                                implicitHeight: 34
                                radius: Math.min(6, theme.radius)
                                color: modelData
                                border.width: chosen ? 3 : 1
                                border.color: chosen ? theme.accent : "#33333a"
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                TapHandler { onTapped: page.brush = parent.modelData }
                            }
                        }
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 8
                        Button { text: qsTr("Fill all keys"); icon.source: "qrc:/icons/paint-bucket.svg"; onClicked: backend.fillDesign(page.brush) }
                        Button { text: qsTr("Random fill"); icon.source: "qrc:/icons/shuffle.svg"; onClicked: backend.randomDesign() }
                        Button { text: qsTr("Clear"); icon.source: "qrc:/icons/eraser.svg"; flat: true; onClicked: clearDialog.open() }
                    }
                }
            }
        }
        LooksSection { Layout.fillWidth: true }
        Item { Layout.preferredHeight: 4 }
    }

    Dialog {
        id: deleteLayerDialog
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: page.current ? qsTr("Delete %1?").arg(page.current.name) : ""
        standardButtons: Dialog.Cancel | Dialog.Ok
        Label { text: qsTr("Its keys go back to showing the layers under it.") }
        onAccepted: { backend.removeLayer(page.layerIndex); page.layerIndex = -1 }
    }
    Dialog {
        id: clearDialog
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Clear the design?")
        standardButtons: Dialog.Cancel | Dialog.Ok
        Label { text: qsTr("Every key of the design goes dark. Layers stay as they are.") }
        onAccepted: backend.clearDesign()
    }
}
