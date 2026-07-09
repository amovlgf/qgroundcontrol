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
import QGroundControl.FactControls
import QGroundControl.FactSystem
import QGroundControl.ScreenTools

SettingsPage {
    property var  _aiConsoleSettings: QGroundControl.settingsManager.aiConsoleSettings
    property real _fieldWidth:        ScreenTools.defaultFontPixelWidth * 42

    SettingsGroupLayout {
        Layout.fillWidth:   true
        heading:            qsTr("AI Console")
        headingDescription: qsTr("The MAVLink Console AI panel sends the active vehicle status snapshot to this OpenAI-compatible endpoint.")

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
            label:                      qsTr("API key")
            fact:                       _aiConsoleSettings.apiKey
            textField.echoMode:         TextInput.Password
        }
    }
}
