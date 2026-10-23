import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls

QGCPopupDialog {
    id: root
    required property var entry
    required property var ridManager
    title: entry ? entry.fact.name : qsTr("RID parameter")
    buttons: Dialog.Save | Dialog.Cancel
    acceptButtonEnabled: entry && ridManager && ridManager.canWrite && !ridManager.locked
    ColumnLayout {
        width: ScreenTools.defaultFontPixelWidth * 45
        QGCLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: root.entry && root.entry.masked ? qsTr("Enter a new password. The current password is not displayed.") : qsTr("Enter a value within the RID parameter limits.")
        }
        QGCTextField {
            id: input
            objectName: "ridParameterInput"
            Layout.fillWidth: true
            text: root.entry && !root.entry.masked ? root.entry.fact.valueString : ""
            echoMode: root.entry && root.entry.masked ? TextInput.Password : TextInput.Normal
            inputMethodHints: root.entry && root.entry.masked ? Qt.ImhSensitiveData | Qt.ImhNoPredictiveText : Qt.ImhNone
        }
        QGCLabel {
            visible: root.entry && root.entry.rebootRequired
            text: qsTr("Restart RID after saving to apply this setting.")
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
    }
    Connections {
        target: QGroundControl.multiVehicleManager
        function onActiveVehicleChanged() {
            input.clear()
            root.close()
        }
    }
    onAccepted: {
        if (entry && ridManager) ridManager.writeParameter(entry.fact.name, input.text)
        input.clear()
    }
    onRejected: input.clear()
    onClosed: input.clear()
}
