/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls
import QGroundControl.Controllers
import QGroundControl.FactControls
import QGroundControl.FactSystem
import QGroundControl.Palette
import QGroundControl.ScreenTools

SettingsPage {
    property var  _aiConsoleSettings: QGroundControl.settingsManager.aiConsoleSettings
    property real _fieldWidth:        ScreenTools.defaultFontPixelWidth * 42
    property bool _isOAuth:           _aiConsoleSettings.authMethod.rawValue === 1
    property string _oauthUrl:        aiAuthController.oauthVerificationUriComplete.length > 0 ? aiAuthController.oauthVerificationUriComplete : aiAuthController.oauthVerificationUri

    QGCPalette {
        id: qgcPal
    }

    MAVLinkConsoleAIController {
        id: aiAuthController
    }

    SettingsGroupLayout {
        Layout.fillWidth:   true
        heading:            qsTr("AI Assistant")
        headingDescription: qsTr("The MAVLink Console AI assistant sends active vehicle status and recent console output to this OpenAI-compatible endpoint.")

        LabelledFactComboBox {
            Layout.fillWidth:           true
            comboBoxPreferredWidth:     _fieldWidth
            label:                      qsTr("Authentication")
            fact:                       _aiConsoleSettings.authMethod
            indexModel:                 false
        }

        LabelledFactTextField {
            Layout.fillWidth:           true
            textFieldPreferredWidth:    _fieldWidth
            label:                      qsTr("Endpoint URL")
            fact:                       _aiConsoleSettings.endpointUrl
        }

        LabelledFactTextField {
            Layout.fillWidth:           true
            textFieldPreferredWidth:    _fieldWidth
            label:                      qsTr("Model")
            fact:                       _aiConsoleSettings.modelName
        }

        LabelledFactTextField {
            Layout.fillWidth:           true
            textFieldPreferredWidth:    _fieldWidth
            label:                      qsTr("API key (optional)")
            fact:                       _aiConsoleSettings.apiKey
            textField.echoMode:         TextInput.Password
            visible:                    !_isOAuth
        }
    }

    SettingsGroupLayout {
        Layout.fillWidth:   true
        heading:            qsTr("OAuth Device Flow")
        headingDescription: qsTr("Use OAuth when the AI endpoint expects a bearer access token from an authorization provider.")
        visible:            _isOAuth

        LabelledFactTextField {
            Layout.fillWidth:           true
            textFieldPreferredWidth:    _fieldWidth
            label:                      qsTr("Device authorization URL")
            fact:                       _aiConsoleSettings.oauthDeviceAuthorizationUrl
        }

        LabelledFactTextField {
            Layout.fillWidth:           true
            textFieldPreferredWidth:    _fieldWidth
            label:                      qsTr("Token URL")
            fact:                       _aiConsoleSettings.oauthTokenUrl
        }

        LabelledFactTextField {
            Layout.fillWidth:           true
            textFieldPreferredWidth:    _fieldWidth
            label:                      qsTr("Client ID")
            fact:                       _aiConsoleSettings.oauthClientId
        }

        LabelledFactTextField {
            Layout.fillWidth:           true
            textFieldPreferredWidth:    _fieldWidth
            label:                      qsTr("Scope")
            fact:                       _aiConsoleSettings.oauthScope
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: ScreenTools.defaultFontPixelWidth

            QGCButton {
                text: aiAuthController.oauthBusy ? qsTr("Authorizing...") : qsTr("Authorize")
                enabled: !aiAuthController.oauthBusy
                onClicked: aiAuthController.startOAuthDeviceAuthorization()
            }

            QGCButton {
                text: qsTr("Cancel")
                enabled: aiAuthController.oauthBusy
                onClicked: aiAuthController.cancelOAuthAuthorization()
            }

            QGCButton {
                text: qsTr("Clear Token")
                enabled: !aiAuthController.oauthBusy && aiAuthController.oauthAuthorized
                onClicked: aiAuthController.clearOAuthTokens()
            }
        }

        QGCLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: aiAuthController.oauthAuthorized
                  ? qsTr("OAuth authorized.")
                  : qsTr("OAuth is not authorized.")
        }

        QGCLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Expires: %1").arg(aiAuthController.oauthExpiresAtText)
            visible: aiAuthController.oauthAuthorized && aiAuthController.oauthExpiresAtText.length > 0
        }

        QGCLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: aiAuthController.oauthStatusText
            visible: text.length > 0
            color: qgcPal.warningText
        }

        QGCLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: aiAuthController.oauthMessage
            visible: text.length > 0
        }

        QGCLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("User code: %1").arg(aiAuthController.oauthUserCode)
            visible: aiAuthController.oauthUserCode.length > 0
            font.bold: true
        }

        RowLayout {
            Layout.fillWidth: true
            visible: _oauthUrl.length > 0

            QGCLabel {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: _oauthUrl
            }

            QGCButton {
                text: qsTr("Open")
                onClicked: Qt.openUrlExternally(_oauthUrl)
            }
        }
    }
}
