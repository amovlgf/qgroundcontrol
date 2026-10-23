import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls

Rectangle {
    id: root
    objectName: "ridManagementPage"
    color: QGroundControl.globalPalette.window
    property var vehicle: QGroundControl.multiVehicleManager.activeVehicle
    readonly property var manager: vehicle ? vehicle.ridManager : null

    QGCFlickable {
        anchors.fill: parent
        anchors.margins: ScreenTools.defaultFontPixelWidth
        contentWidth: width
        contentHeight: content.implicitHeight
        clip: true

        ColumnLayout {
            id: content
            width: parent.width
            spacing: ScreenTools.defaultFontPixelHeight

            QGCLabel { text: qsTr("RID activation and parameters"); font.bold: true }
            QGCLabel {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Read the connected RID, then synchronize its activation state from the platform. Changes require the aircraft to be on the ground and disarmed.")
            }
            QGCLabel {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: root.manager ? root.manager.statusText : qsTr("Connect an aircraft to manage RID")
                objectName: "ridManagementStatus"
            }
            RowLayout {
                QGCButton {
                    objectName: "ridRefreshButton"
                    text: qsTr("Read RID devices")
                    enabled: root.manager && root.manager.available && !root.manager.busy
                    onClicked: root.manager.refresh()
                }
                QGCComboBox {
                    objectName: "ridDeviceSelector"
                    visible: root.manager && root.manager.devices.length > 1
                    model: root.manager ? root.manager.devices : []
                    currentIndex: root.manager ? root.manager.selectedDevice : -1
                    enabled: root.manager && !root.manager.busy
                    onActivated: (index) => root.manager.selectDevice(index)
                }
            }
            GridLayout {
                visible: root.manager && root.manager.supported
                columns: 2
                QGCLabel { text: qsTr("Product code (SN)") }
                QGCLabel { text: root.manager ? root.manager.serial || qsTr("Not provisioned") : ""; textFormat: Text.PlainText }
                QGCLabel { text: qsTr("RID module") }
                QGCLabel { text: root.manager ? root.manager.uid + " / " + root.manager.nodeId : "" }
                QGCLabel { text: qsTr("Platform activation") }
                QGCLabel { text: root.manager ? root.manager.platformState : ""; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                QGCLabel { text: qsTr("Effective RID activation") }
                QGCLabel { text: !root.manager ? "" : root.manager.activationState === 1 ? qsTr("Activated") : root.manager.activationState === 0 ? qsTr("Not activated") : qsTr("Unknown") }
            }
            QGCLabel {
                visible: root.manager && root.manager.supported && !root.manager.serial.length
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: root.manager && root.manager.factoryProvisionSupported
                      ? qsTr("This RID has no factory identity. First bind the aircraft product code (SN), then synchronize activation.")
                      : qsTr("This RID firmware does not support factory identity provisioning through QGC. Install matching RID firmware first.")
            }
            QGCButton {
                objectName: "ridProvisionIdentityButton"
                text: qsTr("Provision factory identity")
                visible: root.manager && root.manager.supported && !root.manager.serial.length
                enabled: root.manager && root.manager.canProvision
                onClicked: provisionFactory.open({ ridManager: root.manager, targetUid: root.manager.uid })
            }
            QGCButton {
                objectName: "ridSyncActivationButton"
                text: qsTr("Synchronize activation")
                enabled: root.manager && root.manager.canWrite && root.manager.serial.length === 20
                onClicked: root.manager.syncActivation()
            }
            QGCLabel {
                visible: root.manager && root.manager.locked
                text: qsTr("RID parameters are locked. Activation synchronization remains available.")
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
            Repeater {
                model: root.manager ? root.manager.parameters : []
                delegate: RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    QGCLabel {
                        text: modelData.fact.name
                        Layout.preferredWidth: ScreenTools.defaultFontPixelWidth * 20
                    }
                    QGCLabel {
                        text: modelData.masked ? qsTr("Hidden") : modelData.fact.valueString
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        wrapMode: Text.WrapAnywhere
                    }
                    QGCLabel {
                        text: !modelData.writable ? qsTr("Read only") : modelData.unused ? qsTr("Unused in this profile") : modelData.rebootRequired ? qsTr("Restart required") : ""
                    }
                    QGCButton {
                        visible: modelData.writable && !modelData.action
                        text: qsTr("Edit")
                        enabled: root.manager && root.manager.canWrite && !root.manager.locked
                        onClicked: editorFactory.open({ entry: modelData, ridManager: root.manager })
                    }
                }
            }
            RowLayout {
                visible: root.manager && root.manager.supported
                QGCButton {
                    text: qsTr("Restart RID")
                    enabled: root.manager && root.manager.canWrite
                    onClicked: maintenanceFactory.open({ reset: false, ridManager: root.manager })
                }
                QGCButton {
                    text: qsTr("Restore RID defaults")
                    enabled: root.manager && root.manager.canWrite && !root.manager.locked
                    onClicked: maintenanceFactory.open({ reset: true, ridManager: root.manager })
                }
            }
        }
    }
    QGCPopupDialogFactory { id: editorFactory; dialogComponent: editorComponent }
    Component { id: editorComponent; RidParameterDialog {} }
    QGCPopupDialogFactory { id: provisionFactory; dialogComponent: provisionComponent }
    Component {
        id: provisionComponent
        QGCPopupDialog {
            id: provisionDialog
            objectName: "ridProvisionIdentityDialog"
            required property var ridManager
            required property string targetUid
            readonly property string validationError: ridManager
                ? ridManager.factoryIdentityError(serialInput.text, classInput.currentIndex) : ""
            title: qsTr("Provision factory identity")
            buttons: Dialog.Save | Dialog.Cancel
            acceptButtonEnabled: ridManager && ridManager.canProvision && ridManager.uid === targetUid
                                 && !validationError.length && bindingConfirmed.checked
            ColumnLayout {
                width: ScreenTools.defaultFontPixelWidth * 50
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WrapAnywhere
                    text: qsTr("Confirm RID hardware UID: %1").arg(provisionDialog.targetUid)
                    textFormat: Text.PlainText
                }
                QGCLabel { text: qsTr("Aircraft product code (SN)") }
                QGCTextField {
                    id: serialInput
                    objectName: "ridFactorySerialInput"
                    Layout.fillWidth: true
                    inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                    placeholderText: qsTr("20 ASCII characters, no spaces")
                }
                QGCLabel { text: qsTr("Aircraft class (0–4)") }
                QGCComboBox {
                    id: classInput
                    objectName: "ridFactoryClassInput"
                    model: ["0", "1", "2", "3", "4"]
                    currentIndex: -1
                    Layout.fillWidth: true
                }
                QGCLabel {
                    visible: serialInput.text.length > 0 && provisionDialog.validationError.length > 0
                    text: provisionDialog.validationError
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }
                QGCCheckBox {
                    id: bindingConfirmed
                    objectName: "ridFactoryBindingConfirmed"
                    wrapMode: Text.WordWrap
                    text: qsTr("I verified the UID and SN and confirm this permanent binding")
                    Layout.fillWidth: true
                }
                QGCLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("The product code cannot be edited after provisioning. It survives restart, firmware updates and restoring ordinary defaults. The RID remains unactivated until platform synchronization.")
                }
            }
            Connections {
                target: QGroundControl.multiVehicleManager
                function onActiveVehicleChanged() { provisionDialog.close() }
            }
            onAccepted: {
                if (ridManager && bindingConfirmed.checked)
                    ridManager.provisionFactoryIdentity(targetUid, serialInput.text, classInput.currentIndex)
            }
        }
    }
    QGCPopupDialogFactory { id: maintenanceFactory; dialogComponent: maintenanceComponent }
    Component {
        id: maintenanceComponent
        QGCPopupDialog {
            required property bool reset
            required property var ridManager
            title: reset ? qsTr("Restore RID defaults") : qsTr("Restart RID")
            buttons: Dialog.Ok | Dialog.Cancel
            acceptButtonEnabled: ridManager && ridManager.canWrite && (!reset || !ridManager.locked)
            QGCLabel {
                width: ScreenTools.defaultFontPixelWidth * 45
                wrapMode: Text.WordWrap
                text: reset ? qsTr("Restore RID settings and restart the module? The factory product code and activation are retained.") : qsTr("Restart the RID module to apply saved settings?")
            }
            onAccepted: { if (reset) ridManager.resetDefaults(); else ridManager.reboot() }
        }
    }
}
