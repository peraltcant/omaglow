import QtQuick
import QtQuick.Controls

// The keyboard drawn with an approximation of the chosen effect, so changes
// show before you look down. The firmware's exact animation isn't known, so
// this follows the effect's idea (pulse, wave, ripple, twinkle) with the
// chosen colors, brightness and speed.
Item {
    id: preview
    property int effect: backend.effect
    property bool on: backend.lightingOn
    property real brightness: backend.brightness / 100
    property color color1: backend.color1
    property color color2: backend.color2
    property int colorMode: backend.colorMode
    property int direction: backend.direction
    property real t: 0
    // The custom lighting as the keyboard gets it: the design with its
    // layers, moving when a layer does.
    property var frame: ({})
    readonly property var stillFrame: { backend.design; backend.layers; return backend.frameAt(0) }

    readonly property real unit: Math.min(width / 16.25, height / 6.25)
    implicitHeight: width / 16.25 * 6.25

    // Faster for "Fast", slower for "Slow"; effects without speed use normal.
    readonly property real pace: [1.7, 1.0, 0.6][backend.speed] || 1.0

    Timer {
        interval: 50
        repeat: true
        running: preview.visible && preview.on && preview.effect !== 0
        onTriggered: preview.t += 0.05 * preview.pace
    }

    Timer {
        interval: 62
        repeat: true
        running: preview.visible && preview.on && backend.customLighting && backend.animatedLighting
        onTriggered: preview.frame = backend.frameAt(Date.now())
    }

    function hue(h, value) {
        return Qt.hsva(((h % 1) + 1) % 1, 1, value, 1)
    }

    // Pseudo-random but steady per key, so twinkles don't flicker every frame.
    function noise(code, slot) {
        var x = Math.sin(code * 12.9898 + slot * 78.233) * 43758.5453
        return x - Math.floor(x)
    }

    function keyColor(k) {
        if (!on)
            return "#141418"
        if (backend.customLighting) {
            var painted = (backend.animatedLighting ? frame : stillFrame)[String(k.code)]
            if (!painted) return "#141418"
            var c = Qt.color(painted)
            return Qt.rgba(c.r * brightness, c.g * brightness, c.b * brightness, 1)
        }
        var cx = (k.x + k.width / 2) / 16.25, cy = (k.y + 0.5) / 6.25
        var base = colorMode === 2 ? hue(t * 0.2 + cx * 0.1, 1) : color1
        var level = 1
        switch (effect) {
        case 0: // static
            base = color1; break
        case 1: { // breathing
            level = 0.5 + 0.5 * Math.sin(t * 3)
            if (colorMode === 1) base = Math.floor(t * 3 / (2 * Math.PI)) % 2 ? color2 : color1
            break
        }
        case 2: // color cycle
            base = hue(t * 0.15, 1); break
        case 3: { // reactive: a few keys flash, the rest stay dark
            var slot = Math.floor(t * 2)
            level = noise(k.code, slot) > 0.9 ? 1 - (t * 2 - slot) : 0.05
            break
        }
        case 4: { // rainbow, along the chosen direction
            var along = [cx, (cx + cy) / 2, cy, (cy - cx) / 2, -cx, -(cx + cy) / 2, -cy, (cx - cy) / 2][Math.max(0, direction)] || cx
            base = hue(along - t * 0.3, 1); break
        }
        case 5: { // ripple from the middle
            var d = Math.sqrt(Math.pow((cx - 0.5) * 2.6, 2) + Math.pow(cy - 0.5, 2))
            base = hue(d - t * 0.4, 1)
            level = 0.35 + 0.65 * Math.max(0, Math.cos((d - t * 0.6) * 9)); break
        }
        case 6: { // starry night
            var s = Math.floor(t * 1.5 + noise(k.code, 1) * 10)
            level = noise(k.code, s) > 0.75 ? 1 : 0.08; break
        }
        case 7: { // quicksand: bands drifting down (or up)
            var dir = direction === 6 ? -1 : 1
            base = hue(cy * dir * 0.8 - t * 0.25, 1); break
        }
        case 8: { // current: a bright wave across
            var w = Math.abs(((cx - t * 0.35) % 1 + 1) % 1 - 0.5)
            level = Math.max(0.08, 1 - w * 4); break
        }
        case 9: { // raindrop: drops falling down columns
            var col = Math.floor(cx * 16)
            var head = ((t * 0.8 + noise(col, 3)) % 1) * 1.3
            level = Math.abs(cy - head) < 0.12 ? 1 : 0.06; break
        }
        }
        level *= Math.max(0.12, brightness)
        return Qt.rgba(base.r * level, base.g * level, base.b * level, 1)
    }

    Item {
        anchors.centerIn: parent
        width: preview.unit * 16.25
        height: preview.unit * 6.25
        Repeater {
            model: backend.keys
            Rectangle {
                required property var modelData
                x: modelData.x * preview.unit + 1.5
                y: modelData.y * preview.unit + 1.5
                width: modelData.width * preview.unit - 3
                height: preview.unit - 3
                radius: Math.min(3, theme.radius)
                color: { preview.t; return preview.keyColor(modelData) }
                border.color: "#26262c"
            }
        }
    }
}
