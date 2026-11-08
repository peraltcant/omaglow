import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Save the current lighting as a named look, and bring saved ones back.
Section {
    id: section
    icon: "bookmark"
    title: qsTr("Looks")
    note: qsTr("Save the lighting you have now with a name.")

    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        TextField {
            id: nameField
            Layout.fillWidth: true
            placeholderText: qsTr("Name, like CS2 or Work")
            onAccepted: save()
            function save() {
                var name = text.trim()
                if (name.length === 0) return
                // Saving under a name in use replaces that look, so ask first.
                if (backend.looks.some(l => l.name.toLowerCase() === name.toLowerCase())) {
                    replaceDialog.name = name
                    replaceDialog.open()
                    return
                }
                backend.saveLook(name)
                text = ""
            }
        }
        Button {
            text: qsTr("Save look"); icon.source: "qrc:/icons/save.svg"
            enabled: nameField.text.trim().length > 0
            onClicked: nameField.save()
        }
    }

    Label {
        visible: backend.looks.length === 0
        opacity: 0.6
        text: qsTr("No looks yet.")
    }
    Repeater {
        model: backend.looks
        RowLayout {
            required property var modelData
            required property int index
            Layout.fillWidth: true
            spacing: 8
            Label { text: modelData.name; font.bold: true; Layout.fillWidth: true; elide: Text.ElideRight }
            Label { visible: modelData.active; text: qsTr("On now"); color: theme.accent }
            Label {
                opacity: 0.6
                text: !modelData.on ? qsTr("Off") : modelData.custom ? qsTr("Custom") : backend.effects[modelData.effect].name
            }
            Button { text: qsTr("Apply"); icon.source: "qrc:/icons/check.svg"; flat: true; onClicked: backend.applyLook(index) }
            Button {
                text: qsTr("Delete"); icon.source: "qrc:/icons/trash-2.svg"
                flat: true
                onClicked: { deleteDialog.index = index; deleteDialog.name = modelData.name; deleteDialog.open() }
            }
        }
    }

    Dialog {
        id: replaceDialog
        property string name
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Replace %1?").arg(name)
        standardButtons: Dialog.Cancel | Dialog.Ok
        Label { text: qsTr("A look with this name exists. It becomes the lighting you have now.") }
        onAccepted: { backend.saveLook(name); nameField.text = "" }
    }
    Dialog {
        id: deleteDialog
        property int index: -1
        property string name
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Delete %1?").arg(name)
        standardButtons: Dialog.Cancel | Dialog.Ok
        Label { text: qsTr("The look is gone for good; the lighting you have now stays.") }
        onAccepted: backend.deleteLook(index)
    }
}
