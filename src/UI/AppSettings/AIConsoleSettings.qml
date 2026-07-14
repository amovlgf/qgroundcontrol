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
    property bool _isChatGpt:         _aiConsoleSettings.authMethod.rawValue === 1

    QGCPalette {
        id: qgcPal
    }

    MAVLinkConsoleAIController {
        id: aiAuthController
    }

    SettingsGroupLayout {
        Layout.fillWidth:   true
        heading:            qsTr("AI Assistant")
        headingDescription: qsTr("The MAVLink Console AI assistant sends active vehicle status and recent console output to the selected provider.")

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
            visible:                    !_isChatGpt
        }

        LabelledFactTextField {
            Layout.fillWidth:           true
            textFieldPreferredWidth:    _fieldWidth
            label:                      qsTr("Model")
            fact:                       _aiConsoleSettings.modelName
            visible:                    !_isChatGpt
        }

        LabelledFactTextField {
            Layout.fillWidth:           true
            textFieldPreferredWidth:    _fieldWidth
            label:                      qsTr("API key (optional)")
            fact:                       _aiConsoleSettings.apiKey
            textField.echoMode:         TextInput.Password
            visible:                    !_isChatGpt
        }
    }

    SettingsGroupLayout {
        Layout.fillWidth:   true
        heading:            qsTr("ChatGPT Account")
        headingDescription: qsTr("ChatGPT authentication, account state, and models are managed by the local Codex App Server.")
        visible:            _isChatGpt

        QGCLabel {
            Layout.fillWidth: true
            wrapMode:         Text.WordWrap
            text:             qsTr("Status: %1").arg(aiAuthController.chatGptStatusText.length > 0 ? aiAuthController.chatGptStatusText : qsTr("Not signed in"))
            color:            aiAuthController.chatGptSignedIn ? qgcPal.text : qgcPal.warningText
        }

        QGCLabel {
            Layout.fillWidth: true
            wrapMode:         Text.WordWrap
            text:             aiAuthController.chatGptLoginInProgress
                                  ? qsTr("Login steps:\n1. Click Open login page.\n2. If the browser says device code login is disabled, enable Codex device code authorization in ChatGPT Settings > Security.\n3. Click Continue in the browser and enter the code shown below.")
                                  : qsTr("Before signing in, enable Codex device code authorization in ChatGPT Settings > Security, then click Sign in with ChatGPT.")
            visible:          !aiAuthController.chatGptSignedIn
            color:             qgcPal.warningText
        }

        QGCLabel {
            Layout.fillWidth: true
            wrapMode:         Text.WordWrap
            text:             qsTr("Account: %1").arg(aiAuthController.chatGptAccountEmail.length > 0 ? aiAuthController.chatGptAccountEmail : qsTr("Not available"))
            visible:          aiAuthController.chatGptSignedIn
        }

        QGCLabel {
            Layout.fillWidth: true
            wrapMode:         Text.WordWrap
            text:             qsTr("Plan: %1").arg(aiAuthController.chatGptPlanType.length > 0 ? aiAuthController.chatGptPlanType : qsTr("Not available"))
            visible:          aiAuthController.chatGptSignedIn
        }

        RowLayout {
            Layout.fillWidth: true
            visible:          !aiAuthController.chatGptSignedIn

            QGCButton {
                text:    aiAuthController.chatGptLoginInProgress ? qsTr("Waiting for authorization") : qsTr("Sign in with ChatGPT")
                enabled: !aiAuthController.chatGptLoginInProgress
                onClicked: aiAuthController.startChatGptLogin()
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing:          ScreenTools.defaultFontPixelHeight * 0.5
            visible:          aiAuthController.chatGptLoginInProgress && aiAuthController.chatGptUserCode.length > 0

            QGCLabel {
                Layout.fillWidth: true
                wrapMode:         Text.WordWrap
                text:             qsTr("Open the following page and enter the code:")
            }

            QGCLabel {
                Layout.fillWidth: true
                wrapMode:         Text.WordWrap
                text:             qsTr("Verification URL: %1").arg(aiAuthController.chatGptVerificationUrl)
            }

            QGCLabel {
                Layout.fillWidth: true
                text:             qsTr("Code: %1").arg(aiAuthController.chatGptUserCode)
                font.bold:        true
            }

            RowLayout {
                Layout.fillWidth: true

                QGCButton {
                    text:    qsTr("Open login page")
                    onClicked: aiAuthController.openChatGptLoginPage()
                }

                QGCButton {
                    text:    qsTr("Copy code")
                    onClicked: aiAuthController.copyChatGptUserCode()
                }

                QGCButton {
                    text:    qsTr("Cancel")
                    onClicked: aiAuthController.cancelChatGptLogin()
                }
            }
        }

        LabelledComboBox {
            id:                         chatGptModelCombo
            Layout.fillWidth:           true
            comboBoxPreferredWidth:     _fieldWidth
            label:                      qsTr("Model")
            model:                      aiAuthController.chatGptModelNames
            currentIndex:               aiAuthController.chatGptModelIndex
            visible:                    aiAuthController.chatGptSignedIn
            onActivated: (index) => aiAuthController.selectChatGptModel(index)
        }

        QGCButton {
            text:    qsTr("Sign out")
            enabled: aiAuthController.chatGptSignedIn && !aiAuthController.chatGptLoginInProgress
            visible: enabled
            onClicked: aiAuthController.signOutChatGpt()
        }
    }
}
