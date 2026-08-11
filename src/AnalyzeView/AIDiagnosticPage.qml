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
import QGroundControl.Palette
import QGroundControl.ScreenTools

AnalyzePage {
    id: root

    pageName:      qsTr("AI Flight Diagnostics")
    pageComponent: pageComponent
    allowPopout:   true

    QGCPalette {
        id: qgcPal
    }

    AIDiagnosticController {
        id: aiController
    }

    Component {
        id: pageComponent

        ColumnLayout {
            id:      pageRoot
            width:   availableWidth
            height:  availableHeight
            spacing: ScreenTools.defaultFontPixelHeight * 0.5

            readonly property string _initialTranscript: qsTr("Ask a diagnostic question about a connected PX4 vehicle, or attach PX4 console evidence for offline read-only analysis.")
            property bool _transcriptStarted: false
            property bool _answerStreaming:   false

            function appendTranscript(speaker, message) {
                const trimmedMessage = (message || "").trim()
                if (trimmedMessage.length === 0) {
                    return
                }

                _answerStreaming = false
                if (!_transcriptStarted) {
                    transcript.text = ""
                    _transcriptStarted = true
                }
                transcript.text += speaker + ": " + trimmedMessage + "\n\n"
                transcriptScrollTimer.start()
            }

            function appendAnswerDelta(delta) {
                if (!delta || delta.length === 0) {
                    return
                }
                if (!_transcriptStarted) {
                    transcript.text = ""
                    _transcriptStarted = true
                }
                if (!_answerStreaming) {
                    transcript.text += qsTr("AI") + ": "
                    _answerStreaming = true
                }
                transcript.text += delta
                transcriptScrollTimer.start()
            }

            function finalizeAnswer(answer) {
                const trimmedAnswer = (answer || "").trim()
                if (trimmedAnswer.length === 0) {
                    return
                }
                if (_answerStreaming) {
                    const prefix = qsTr("AI") + ": "
                    const prefixIndex = transcript.text.lastIndexOf(prefix)
                    if (prefixIndex >= 0) {
                        transcript.text = transcript.text.substring(0, prefixIndex) + prefix + trimmedAnswer + "\n\n"
                    } else {
                        appendTranscript(qsTr("AI"), trimmedAnswer)
                    }
                    _answerStreaming = false
                } else {
                    appendTranscript(qsTr("AI"), trimmedAnswer)
                }
                transcriptScrollTimer.start()
            }

            function resetSession() {
                _transcriptStarted = false
                _answerStreaming = false
                transcript.text = _initialTranscript
                consoleEvidence.text = ""
                transcriptScrollTimer.start()
            }

            function askQuestion() {
                const question = questionInput.text.trim()
                if (question.length === 0) {
                    return
                }
                appendTranscript(qsTr("You"), question)
                questionInput.text = ""
                aiController.askWithEvidence(question, consoleEvidence.text)
            }

            Connections {
                target: aiController

                function onAnswerReady(answer) {
                    pageRoot.appendTranscript(qsTr("AI"), answer)
                }

                function onAnswerDelta(delta) {
                    pageRoot.appendAnswerDelta(delta)
                }

                function onAnswerFinalized(answer) {
                    pageRoot.finalizeAnswer(answer)
                }

                function onRequestFailed(errorText) {
                    pageRoot.appendTranscript(qsTr("AI"), errorText)
                }

                function onConversationCleared() {
                    pageRoot.resetSession()
                }
            }

            Timer {
                id:       transcriptScrollTimer
                interval: 0
                repeat:   false

                onTriggered: {
                    if (transcriptFlickable.contentHeight > transcriptFlickable.height) {
                        transcriptFlickable.contentY = transcriptFlickable.contentHeight - transcriptFlickable.height
                    } else {
                        transcriptFlickable.contentY = 0
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing:          ScreenTools.defaultFontPixelWidth * 0.75

                QGCLabel {
                    Layout.fillWidth: true
                    text:             aiController.activeVehicleStatusText
                    color:            aiController.activeVehicleSupported ? qgcPal.text : qgcPal.warningText
                    elide:            Text.ElideRight
                }

                QGCButton {
                    text:      qsTr("Settings")
                    enabled:   !aiController.busy
                    onClicked: mainWindow.showSettingsTool(qsTr("AI Assistant"))
                }

                QGCButton {
                    text:      qsTr("Clear")
                    enabled:   (pageRoot._transcriptStarted || consoleEvidence.text.length > 0) && !aiController.busy
                    onClicked: aiController.clearConversation()
                }
            }

            QGCLabel {
                Layout.fillWidth: true
                visible:          !aiController.configured
                text:             qsTr("Configure an AI endpoint or sign in with ChatGPT before asking a question.")
                color:            qgcPal.warningText
                wrapMode:         Text.WordWrap
            }

            QGCLabel {
                Layout.fillWidth: true
                visible:          aiController.busy
                text:             aiController.pendingActionAvailable ? qsTr("Waiting for tool confirmation…") : qsTr("Running read-only diagnosis…")
                wrapMode:         Text.WordWrap
            }

            Rectangle {
                Layout.fillWidth:       true
                Layout.preferredHeight: approvalColumn.implicitHeight + ScreenTools.defaultFontPixelHeight
                visible:                aiController.pendingActionAvailable
                color:                  qgcPal.window
                border.color:           qgcPal.warningText
                border.width:           1
                radius:                 3

                ColumnLayout {
                    id:              approvalColumn
                    anchors.fill:    parent
                    anchors.margins: ScreenTools.defaultFontPixelWidth * 0.75
                    spacing:         ScreenTools.defaultFontPixelHeight * 0.3

                    QGCLabel {
                        Layout.fillWidth: true
                        text:             aiController.pendingActionTitle
                        font.bold:        true
                        color:            qgcPal.warningText
                    }

                    QGCLabel {
                        Layout.fillWidth: true
                        text:             aiController.pendingActionDescription
                        wrapMode:         Text.WordWrap
                    }

                    QGCLabel {
                        Layout.fillWidth: true
                        text:             aiController.pendingActionCommand
                        wrapMode:         QGCLabel.WrapAnywhere
                        font.family:      ScreenTools.fixedFontFamily
                        font.pointSize:   ScreenTools.smallFontPointSize
                    }

                    QGCLabel {
                        Layout.fillWidth: true
                        text:             aiController.pendingActionRisk
                        wrapMode:         Text.WordWrap
                        color:            qgcPal.warningText
                    }

                    RowLayout {
                        Layout.fillWidth: true

                        Item { Layout.fillWidth: true }

                        QGCButton {
                            text:      qsTr("Reject")
                            onClicked: aiController.rejectPendingAction()
                        }

                        QGCButton {
                            text:      qsTr("Approve")
                            onClicked: aiController.approvePendingAction()
                        }
                    }
                }
            }

            QGCFlickable {
                id:                transcriptFlickable
                Layout.fillWidth:  true
                Layout.fillHeight: true
                contentWidth:      transcript.width
                contentHeight:     transcript.height
                clip:              true

                TextArea.flickable: TextArea {
                    id:                transcript
                    width:             transcriptFlickable.width
                    readOnly:          true
                    text:              pageRoot._initialTranscript
                    textFormat:        TextEdit.PlainText
                    wrapMode:          TextEdit.Wrap
                    color:             qgcPal.text
                    selectedTextColor: qgcPal.windowShade
                    selectionColor:    qgcPal.text
                    font.pointSize:    ScreenTools.defaultFontPointSize

                    background: Rectangle { color: qgcPal.windowShade }
                }
            }

            ColumnLayout {
                id:               evidenceSection
                Layout.fillWidth: true
                spacing:          ScreenTools.defaultFontPixelHeight * 0.25
                property bool expanded: false

                RowLayout {
                    Layout.fillWidth: true

                    QGCButton {
                        text: evidenceSection.expanded ? qsTr("Hide Console evidence") : qsTr("Attach Console evidence…")
                        enabled: !aiController.busy
                        onClicked: evidenceSection.expanded = !evidenceSection.expanded
                    }

                    QGCLabel {
                        Layout.fillWidth: true
                        visible:          !evidenceSection.expanded && consoleEvidence.text.length > 0
                        text:             qsTr("Console evidence attached (%1 characters)").arg(consoleEvidence.text.length)
                        color:            qgcPal.text
                        elide:            Text.ElideRight
                    }
                }

                QGCLabel {
                    Layout.fillWidth: true
                    visible:          evidenceSection.expanded
                    text:             qsTr("Only pasted text is sent. Remove secrets and unrelated logs first; QGroundControl never collects Console output in the background.")
                    color:            qgcPal.warningText
                    wrapMode:         Text.WordWrap
                    font.pointSize:   ScreenTools.smallFontPointSize
                }

                TextArea {
                    id:                     consoleEvidence
                    Layout.fillWidth:       true
                    Layout.preferredHeight: ScreenTools.defaultFontPixelHeight * 6
                    visible:                evidenceSection.expanded
                    enabled:                !aiController.busy
                    placeholderText:        qsTr("Paste PX4 Console evidence here…")
                    textFormat:             TextEdit.PlainText
                    wrapMode:               TextEdit.Wrap
                    selectByMouse:          true
                    font.family:            ScreenTools.fixedFontFamily
                    font.pointSize:         ScreenTools.smallFontPointSize

                    background: Rectangle {
                        color:        qgcPal.window
                        border.color: consoleEvidence.activeFocus ? qgcPal.buttonHighlight : qgcPal.text
                        border.width: consoleEvidence.activeFocus ? 2 : 1
                        radius:       2
                    }
                }

                QGCLabel {
                    Layout.fillWidth: true
                    visible:          evidenceSection.expanded
                    text: consoleEvidence.text.length > 12000
                          ? qsTr("%1 characters; only the last 12,000 will be sent.").arg(consoleEvidence.text.length)
                          : qsTr("%1 / 12,000 characters").arg(consoleEvidence.text.length)
                    font.pointSize: ScreenTools.smallFontPointSize
                }
            }

            RowLayout {
                Layout.fillWidth: true

                TextArea {
                    id:                     questionInput
                    Layout.fillWidth:       true
                    Layout.preferredHeight: Math.min(ScreenTools.implicitTextFieldHeight * 3,
                                                     Math.max(ScreenTools.implicitTextFieldHeight,
                                                              contentHeight + topPadding + bottomPadding))
                    enabled:                aiController.configured && !aiController.busy
                    placeholderText:        qsTr("Ask about PX4 status, health, sensors, parameters, or attached evidence…")
                    textFormat:             TextEdit.PlainText
                    wrapMode:               TextEdit.Wrap
                    selectByMouse:          true
                    inputMethodHints:       Qt.ImhMultiLine
                    color:                  qgcPal.text
                    selectedTextColor:      qgcPal.windowShade
                    selectionColor:         qgcPal.text
                    font.pointSize:         ScreenTools.defaultFontPointSize
                    padding:                ScreenTools.defaultFontPixelWidth * 0.75
                    activeFocusOnPress:     true

                    background: Rectangle {
                        color:        qgcPal.window
                        border.color: questionInput.activeFocus ? qgcPal.buttonHighlight : qgcPal.buttonBorder
                        border.width: questionInput.activeFocus ? 2 : 1
                        radius:       2
                    }
                }

                QGCButton {
                    text:      qsTr("Ask")
                    visible:   !aiController.busy
                    enabled: aiController.configured
                             && questionInput.text.trim().length > 0
                             && (aiController.activeVehicleSupported
                                 || (!aiController.activeVehicleAvailable && consoleEvidence.text.trim().length > 0))
                    onClicked: pageRoot.askQuestion()
                }

                QGCButton {
                    text:      qsTr("Cancel")
                    visible:   aiController.busy
                    enabled:   aiController.busy
                    onClicked: aiController.cancel()
                }
            }
        }
    }
}
