pragma ComponentBehavior: Bound

import QGroundControl
import QGroundControl.Controls
import QGroundControl.FactControls

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root

    readonly property bool fixedLocation: fixedSettings.locationType.rawValue === RemoteIDSettings.LocationType.FIXED
    readonly property var fixedSettings: QGroundControl.settingsManager.remoteIDSettings
    required property var manager

    objectName: "uomReportingPanel"
    spacing: ScreenTools.defaultFontPixelHeight * 0.5

    QGCLabel {
        font.bold: true
        text: qsTr("UOM flight reporting")
    }

    QGCLabel {
        Layout.fillWidth: true
        text: qsTr("Reporting runs automatically while QGC is open. Local readiness and platform acknowledgements are shown separately.")
        wrapMode: Text.WordWrap
    }

    GridLayout {
        Layout.fillWidth: true
        columnSpacing: ScreenTools.defaultFontPixelWidth * 2
        columns: 2

        QGCLabel {
            text: qsTr("Aircraft SN")
        }

        QGCLabel {
            Layout.fillWidth: true
            objectName: "uomReportingAircraftId"
            text: root.manager && root.manager.aircraftId.length ? root.manager.aircraftId : qsTr("Unknown")
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
        }

        QGCLabel {
            text: qsTr("Device activation")
        }

        QGCLabel {
            text: root.manager && root.manager.activated ? qsTr("Activated") : qsTr("Not confirmed")
        }

        QGCLabel {
            text: qsTr("Local readiness")
        }

        QGCLabel {
            objectName: "uomReportingLocalReady"
            text: root.manager && root.manager.localReady ? qsTr("Ready") : qsTr("Not ready")
        }

        QGCLabel {
            text: qsTr("Network reporting")
        }

        QGCLabel {
            objectName: "uomReportingNetworkHealthy"
            text: root.manager && root.manager.networkHealthy ? qsTr("Confirmed") : qsTr("Not confirmed")
        }

        QGCLabel {
            text: qsTr("Reporting model")
        }

        QGCLabel {
            Layout.fillWidth: true
            objectName: "uomReportingModel"
            text: root.manager ? root.manager.reportingModel : qsTr("Unknown")
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
        }

        QGCLabel {
            text: qsTr("Flight-controller protocol")
        }

        QGCLabel {
            text: root.manager && root.manager.protocolCompatible ? qsTr("Compatible") : qsTr("Not confirmed")
        }

        QGCLabel {
            text: qsTr("RID broadcast")
        }

        QGCLabel {
            text: root.manager && root.manager.broadcastReady ? qsTr("Ready") : qsTr("Not ready")
        }

        QGCLabel {
            text: qsTr("Pending reports")
        }

        QGCLabel {
            objectName: "uomReportingPendingCount"
            text: root.manager ? String(root.manager.pendingReports) : "0"
        }

        QGCLabel {
            text: qsTr("Held reports")
        }

        QGCLabel {
            objectName: "uomReportingHeldCount"
            text: root.manager ? String(root.manager.heldReports) : "0"
        }

        QGCLabel {
            text: qsTr("Last platform acknowledgement")
        }

        QGCLabel {
            objectName: "uomReportingLastAck"
            text: root.manager && root.manager.lastAckAgeMs >= 0 && root.manager.lastAckAgeMs < 4294967295 ? qsTr("%1 seconds ago").arg(Math.floor(root.manager.lastAckAgeMs / 1000)) : qsTr("None recorded")
        }
    }

    QGCLabel {
        Layout.fillWidth: true
        objectName: "uomReportingLocalStatus"
        text: root.manager ? root.manager.localStatusText : qsTr("Connect an aircraft to inspect local readiness")
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
    }

    QGCLabel {
        Layout.fillWidth: true
        objectName: "uomReportingNetworkStatus"
        text: root.manager ? root.manager.networkStatusText : qsTr("Saved reports continue in the background")
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
    }

    RowLayout {
        QGCLabel {
            text: qsTr("Operator position source")
        }

        FactComboBox {
            fact: root.fixedSettings.locationType
            indexModel: false
            objectName: "uomOperatorPositionSource"
            sizeToContents: true
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        visible: root.fixedLocation

        QGCLabel {
            Layout.fillWidth: true
            text: qsTr("Fixed operator position: verify this operating site and its WGS84 ellipsoid altitude. Zero values are valid. Editing a coordinate clears the confirmation.")
            wrapMode: Text.WordWrap
        }

        GridLayout {
            columns: 2

            QGCLabel {
                text: qsTr("Latitude")
            }

            FactTextField {
                fact: root.fixedSettings.latitudeFixed
                objectName: "uomFixedLatitude"
            }

            QGCLabel {
                text: qsTr("Longitude")
            }

            FactTextField {
                fact: root.fixedSettings.longitudeFixed
                objectName: "uomFixedLongitude"
            }

            QGCLabel {
                text: qsTr("WGS84 ellipsoid altitude")
            }

            FactTextField {
                fact: root.fixedSettings.altitudeFixed
                objectName: "uomFixedAltitude"
            }
        }

        FactCheckBox {
            Layout.fillWidth: true
            fact: root.fixedSettings.fixedPositionConfirmed
            objectName: "uomFixedPositionConfirmed"
            text: qsTr("I verified the fixed position and WGS84 ellipsoid altitude for this site")
            wrapMode: Text.WordWrap
        }
    }

    RowLayout {
        QGCButton {
            enabled: !!root.manager
            objectName: "uomReloadReportingConfiguration"
            text: qsTr("Reload manufacturer configuration")

            onClicked: root.manager.reloadReportingConfiguration()
        }

        QGCButton {
            enabled: !!root.manager
            objectName: "uomExportReportingDiagnostics"
            text: qsTr("Export reporting diagnostics")

            onClicked: diagnosticsDialog.openForSave()
        }
    }

    QGCLabel {
        Layout.fillWidth: true
        text: qsTr("Diagnostic exports contain reporting status and counters, without credentials or raw flight payloads.")
        wrapMode: Text.WordWrap
    }

    QGCFileDialog {
        id: diagnosticsDialog

        defaultSuffix: "json"
        folder: QGroundControl.settingsManager.appSettings.savePath.rawValue
        nameFilters: [qsTr("JSON files (*.json)")]
        title: qsTr("Export reporting diagnostics")

        onAcceptedForSave: file => {
            if (root.manager && !root.manager.exportReportingDiagnostics(QGCFileDialogController.localFileToUrl(file))) {
                QGroundControl.showMessageDialog(root, qsTr("Reporting diagnostics"), qsTr("The diagnostic file could not be saved."));
            }
        }
    }
}
