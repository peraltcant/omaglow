import QtQuick
import QtQuick.Controls.impl

// A bundled Lucide icon, drawn in a single color.
IconImage {
    property string glyph // IconImage already has name, for theme icons
    property real size: 18
    source: glyph !== "" ? "qrc:/icons/" + glyph + ".svg" : ""
    sourceSize: Qt.size(size, size)
    color: "white"
}
