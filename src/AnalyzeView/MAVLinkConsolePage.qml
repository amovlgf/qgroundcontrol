/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
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
import QGroundControl.ScreenTools
import QGroundControl.Controllers

AnalyzePage {
    id: root
    pageComponent: pageComponent
    pageDescription: qsTr("Provides a connection to the vehicle's system shell.")
    allowPopout: true

    property bool isLoaded: false

    // Key input on mobile is handled differently, so use a separate command input text field.
    // E.g. for android see https://bugreports.qt.io/browse/QTBUG-40803
    readonly property bool _separateCommandInput: ScreenTools.isMobile

    MAVLinkConsoleController { id: conController }
    MAVLinkConsoleAIController { id: aiController }

    Component {
        id: pageComponent

        GridLayout {
            id: pageRoot
            height: availableHeight
            width: availableWidth
            columns: ScreenTools.isMobile ? 1 : 2
            columnSpacing: ScreenTools.defaultFontPixelWidth
            rowSpacing: ScreenTools.defaultFontPixelHeight * 0.5

            property int _consoleOutputLen: 0
            property bool _aiTranscriptStarted: false
            property bool _aiAnswerStreaming: false
            property bool _aiPanelCollapsed: false
            readonly property real _aiCollapsedHandleWidth: ScreenTools.implicitButtonHeight + (ScreenTools.defaultFontPixelWidth * 1.5)
            readonly property string _aiInitialText: qsTr("Ask about the active vehicle status or recent MAVLink Console output.")

            function scrollToBottom() {
                if (flickable.contentHeight > flickable.height) {
                    flickable.contentY = flickable.contentHeight - flickable.height
                }
            }

            function getCommand() { return textConsole.getText(_consoleOutputLen, textConsole.length) }

            function getCommandAndClear() {
                const command = getCommand()
                textConsole.remove(_consoleOutputLen, textConsole.length)
                return command
            }

            function pasteFromClipboard() {
                // we need to handle a few cases here:
                // in the general form we have: <command_pre><cursor><command_post>
                // and the clipboard may contain newlines
                const cursor = textConsole.cursorPosition - _consoleOutputLen
                var command = getCommandAndClear()
                var command_pre = ""
                var command_post = command
                if (cursor > 0) {
                    command_pre = command.substr(0, cursor)
                    command_post = command.substr(cursor)
                }
                var command_leftover = conController.handleClipboard(command_pre) + command_post
                textConsole.insert(textConsole.length, command_leftover)
                textConsole.cursorPosition = textConsole.length - command_post.length
            }

            function askAI() {
                const question = aiQuestion.text.trim()
                if (question.length === 0) {
                    return
                }

                appendAITranscript(qsTr("You"), question)
                aiQuestion.text = ""
                aiController.askWithContext(question, conController.text)
            }

            function appendAITranscript(speaker, message) {
                const trimmedMessage = (message || "").trim()
                if (trimmedMessage.length === 0) {
                    return
                }

                _aiAnswerStreaming = false

                if (!_aiTranscriptStarted) {
                    aiAnswer.text = ""
                    _aiTranscriptStarted = true
                }

                aiAnswer.text += speaker + ": " + trimmedMessage + "\n\n"
                aiScrollTimer.start()
            }

            function appendAIAnswerDelta(delta) {
                if (!delta || delta.length === 0) {
                    return
                }
                if (!_aiTranscriptStarted) {
                    aiAnswer.text = ""
                    _aiTranscriptStarted = true
                }
                if (!_aiAnswerStreaming) {
                    aiAnswer.text += qsTr("AI") + ": "
                    _aiAnswerStreaming = true
                }
                aiAnswer.text += delta
                aiScrollTimer.start()
            }

            function finalizeAIAnswer(answer) {
                if (!answer || answer.trim().length === 0) {
                    return
                }
                if (_aiAnswerStreaming) {
                    const prefix = qsTr("AI") + ": "
                    const prefixIndex = aiAnswer.text.lastIndexOf(prefix)
                    if (prefixIndex >= 0) {
                        aiAnswer.text = aiAnswer.text.substring(0, prefixIndex) + prefix + answer.trim() + "\n\n"
                    } else {
                        appendAITranscript(qsTr("AI"), answer)
                    }
                    _aiAnswerStreaming = false
                } else {
                    appendAITranscript(qsTr("AI"), answer)
                }
                aiScrollTimer.start()
            }

            function resetAITranscript() {
                _aiTranscriptStarted = false
                _aiAnswerStreaming = false
                aiAnswer.text = _aiInitialText
                aiScrollTimer.start()
            }

            function collapseAIPanel() {
                if (ScreenTools.isMobile) {
                    return
                }

                _aiPanelCollapsed = true
                if (!_separateCommandInput) {
                    textConsole.forceActiveFocus()
                }
            }

            function expandAIPanel() {
                _aiPanelCollapsed = false
                aiScrollTimer.start()
            }

            Connections {
                target: conController

                onDataChanged: {
                    if (isLoaded) {
                        // rate-limit updates to reduce CPU load
                        updateTimer.start();
                    }
                }
            }

            Connections {
                target: aiController

                function onAnswerReady(answer) {
                    appendAITranscript(qsTr("AI"), answer)
                }

                function onAnswerDelta(delta) {
                    appendAIAnswerDelta(delta)
                }

                function onAnswerFinalized(answer) {
                    finalizeAIAnswer(answer)
                }

                function onRequestFailed(errorText) {
                    appendAITranscript(qsTr("AI"), errorText)
                }

                function onConversationCleared() {
                    resetAITranscript()
                }
            }

            Timer {
                id: aiScrollTimer
                interval: 0
                running: false
                repeat: false
                onTriggered: {
                    if (aiAnswerFlickable.contentHeight > aiAnswerFlickable.height) {
                        aiAnswerFlickable.contentY = aiAnswerFlickable.contentHeight - aiAnswerFlickable.height
                    } else {
                        aiAnswerFlickable.contentY = 0
                    }
                }
            }

            Timer {
                id: updateTimer
                interval: 30
                running: false
                repeat: false
                onTriggered: {
                    // only update if scroll bar is at the bottom
                    if (flickable.atYEnd) {
                        // backup & restore cursor & command
                        const command = getCommand()
                        const cursor = textConsole.cursorPosition - _consoleOutputLen
                        textConsole.text = conController.text
                        _consoleOutputLen = textConsole.length
                        textConsole.insert(textConsole.length, command)
                        textConsole.cursorPosition = textConsole.length
                        scrollToBottom()
                        if (cursor >= 0) {
                            // We could restore the selection here too...
                            textConsole.cursorPosition = _consoleOutputLen + cursor
                        }
                    } else {
                        updateTimer.start();
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: ScreenTools.isMobile ? availableWidth
                                                            : (pageRoot._aiPanelCollapsed
                                                               ? Math.max(0, availableWidth - pageRoot._aiCollapsedHandleWidth - pageRoot.columnSpacing)
                                                               : availableWidth * 0.64)
                Layout.preferredHeight: ScreenTools.isMobile ? availableHeight * 0.62 : availableHeight

                QGCFlickable {
                    id: flickable
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentWidth: textConsole.width
                    contentHeight: textConsole.height

                    TextArea.flickable: TextArea {
                        id: textConsole
                        width: flickable.width
                        wrapMode: Text.WordWrap
                        readOnly: _separateCommandInput
                        textFormat: TextEdit.RichText
                        inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhMultiLine
                        text: "> "
                        focus: true
                        color: qgcPal.text
                        selectedTextColor: qgcPal.windowShade
                        selectionColor: qgcPal.text
                        font.pointSize: ScreenTools.defaultFontPointSize
                        font.family: ScreenTools.fixedFontFamily

                        Component.onCompleted: {
                            root.isLoaded = true
                            _consoleOutputLen = textConsole.length
                            textConsole.cursorPosition = _consoleOutputLen
                            if (!_separateCommandInput) {
                                textConsole.forceActiveFocus()
                            }
                        }

                        background: Rectangle { color: qgcPal.windowShade }

                        Keys.onPressed: (event) => {
                            // ignore tabs
                            if (event.key == Qt.Key_Tab) {
                                event.accepted = true
                            }

                            // ignore for now
                            if (event.matches(StandardKey.Cut)) {
                                event.accepted = true
                            }

                            if (!event.matches(StandardKey.Copy) &&
                                event.key != Qt.Key_Escape &&
                                event.key != Qt.Key_Insert &&
                                event.key != Qt.Key_Pause &&
                                event.key != Qt.Key_Print &&
                                event.key != Qt.Key_SysReq &&
                                event.key != Qt.Key_Clear &&
                                event.key != Qt.Key_Home &&
                                event.key != Qt.Key_End &&
                                event.key != Qt.Key_Left &&
                                event.key != Qt.Key_Up &&
                                event.key != Qt.Key_Right &&
                                event.key != Qt.Key_Down &&
                                event.key != Qt.Key_PageUp &&
                                event.key != Qt.Key_PageDown &&
                                event.key != Qt.Key_Shift &&
                                event.key != Qt.Key_Control &&
                                event.key != Qt.Key_Meta &&
                                event.key != Qt.Key_Alt &&
                                event.key != Qt.Key_AltGr &&
                                event.key != Qt.Key_CapsLock &&
                                event.key != Qt.Key_NumLock &&
                                event.key != Qt.Key_ScrollLock &&
                                event.key != Qt.Key_Super_L &&
                                event.key != Qt.Key_Super_R &&
                                event.key != Qt.Key_Menu &&
                                event.key != Qt.Key_Hyper_L &&
                                event.key != Qt.Key_Hyper_R &&
                                event.key != Qt.Key_Direction_L &&
                                event.key != Qt.Key_Direction_R) {
                                // Note: dead keys do not generate keyPressed event on linux, see
                                // https://bugreports.qt.io/browse/QTBUG-79216

                                scrollToBottom()

                                // ensure cursor position is at an editable region
                                if (textConsole.selectionStart < _consoleOutputLen) {
                                    textConsole.select(_consoleOutputLen, textConsole.selectionEnd)
                                }

                                if (textConsole.cursorPosition < _consoleOutputLen) {
                                    textConsole.cursorPosition = textConsole.length
                                }
                            }

                            switch (event.key) {
                            case Qt.Key_Left:
                                // don't move beyond current command
                                if (textConsole.cursorPosition == _consoleOutputLen) {
                                    event.accepted = true
                                }
                                break;
                            case Qt.Key_Backspace:
                                if (textConsole.cursorPosition <= _consoleOutputLen) {
                                    event.accepted = true
                                }
                                break;
                            case Qt.Key_Enter:
                            case Qt.Key_Return:
                                conController.sendCommand(getCommandAndClear())
                                event.accepted = true
                                break;
                            default:
                                break;
                            }

                            if (event.matches(StandardKey.Paste)) {
                                pasteFromClipboard()
                                event.accepted = true
                            }

                            // command history
                            if (event.key == Qt.Key_Up) {
                                const command = conController.historyUp(getCommandAndClear())
                                textConsole.insert(textConsole.length, command)
                                textConsole.cursorPosition = textConsole.length
                                event.accepted = true
                            } else if (event.key == Qt.Key_Down) {
                                const command = conController.historyDown(getCommandAndClear())
                                textConsole.insert(textConsole.length, command)
                                textConsole.cursorPosition = textConsole.length
                                event.accepted = true
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: _separateCommandInput

                    QGCTextField {
                        id: commandInput
                        Layout.fillWidth: true
                        placeholderText: qsTr("Enter Commands here...")
                        inputMethodHints: Qt.ImhNoAutoUppercase
                        onAccepted: sendCommand()

                        function sendCommand() {
                            conController.sendCommand(text)
                            text = ""
                            scrollToBottom()
                        }
                    }

                    QGCButton {
                        text: qsTr("Send")
                        onClicked: commandInput.sendCommand()
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: ScreenTools.isMobile ? availableWidth : availableWidth * 0.36
                Layout.preferredHeight: ScreenTools.isMobile ? availableHeight * 0.38 : availableHeight
                Layout.minimumWidth: ScreenTools.isMobile ? 0 : ScreenTools.defaultFontPixelWidth * 34
                visible: ScreenTools.isMobile || !pageRoot._aiPanelCollapsed
                color: qgcPal.windowShade
                border.color: qgcPal.buttonBorder
                border.width: 1

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: ScreenTools.defaultFontPixelWidth
                    spacing: ScreenTools.defaultFontPixelHeight * 0.5

                    RowLayout {
                        Layout.fillWidth: true

                        QGCLabel {
                            Layout.fillWidth: true
                            text: qsTr("AI Assistant")
                            font.bold: true
                        }

                        QGCButton {
                            text: ">"
                            Layout.preferredWidth: ScreenTools.implicitButtonHeight
                            Layout.preferredHeight: ScreenTools.implicitButtonHeight
                            visible: !ScreenTools.isMobile
                            onClicked: pageRoot.collapseAIPanel()
                            ToolTip.visible: hovered
                            ToolTip.delay: 500
                            ToolTip.text: qsTr("Hide AI Assistant")
                        }

                        QGCButton {
                            text: qsTr("Settings")
                            enabled: !aiController.busy
                            onClicked: mainWindow.showSettingsTool(qsTr("AI Assistant"))
                        }

                        QGCButton {
                            text: qsTr("Clear")
                            enabled: pageRoot._aiTranscriptStarted && !aiController.busy
                            onClicked: aiController.clearConversation()
                        }
                    }

                    QGCLabel {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: !aiController.configured
                              ? qsTr("Configure AI Assistant settings. If OAuth is selected, authorize OAuth first.")
                              : (QGroundControl.multiVehicleManager.activeVehicle ? "" : qsTr("No active vehicle. Using console output only."))
                        visible: text.length > 0
                        color: qgcPal.warningText
                    }

                    QGCLabel {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: aiController.pendingActionAvailable ? qsTr("Waiting for confirmation...")
                              : (aiController.busy ? qsTr("Waiting for response...") : "")
                        visible: text.length > 0
                        color: qgcPal.text
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: pendingActionColumn.implicitHeight + (ScreenTools.defaultFontPixelHeight * 1.4)
                        visible: aiController.pendingActionAvailable
                        color: qgcPal.window
                        border.color: qgcPal.warningText
                        border.width: 1
                        radius: 3

                        ColumnLayout {
                            id: pendingActionColumn
                            anchors.fill: parent
                            anchors.margins: ScreenTools.defaultFontPixelWidth * 0.75
                            spacing: ScreenTools.defaultFontPixelHeight * 0.35

                            QGCLabel {
                                Layout.fillWidth: true
                                text: aiController.pendingActionTitle
                                font.bold: true
                                color: qgcPal.warningText
                            }

                            QGCLabel {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                text: aiController.pendingActionDescription
                            }

                            QGCLabel {
                                Layout.fillWidth: true
                                wrapMode: QGCLabel.WrapAnywhere
                                text: aiController.pendingActionCommand
                                font.family: ScreenTools.fixedFontFamily
                                font.pointSize: ScreenTools.smallFontPointSize
                            }

                            QGCLabel {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                text: aiController.pendingActionRisk
                                color: qgcPal.warningText
                            }

                            RowLayout {
                                Layout.fillWidth: true

                                Item { Layout.fillWidth: true }

                                QGCButton {
                                    text: qsTr("Reject")
                                    enabled: aiController.pendingActionAvailable
                                    onClicked: aiController.rejectPendingAction()
                                }

                                QGCButton {
                                    text: qsTr("Approve")
                                    enabled: aiController.pendingActionAvailable
                                    onClicked: aiController.approvePendingAction()
                                }
                            }
                        }
                    }

                    QGCFlickable {
                        id: aiAnswerFlickable
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        contentWidth: aiAnswer.width
                        contentHeight: aiAnswer.height
                        clip: true

                        TextArea.flickable: TextArea {
                            id: aiAnswer
                            width: aiAnswerFlickable.width
                            wrapMode: Text.WordWrap
                            readOnly: true
                            textFormat: TextEdit.PlainText
                            text: pageRoot._aiInitialText
                            color: qgcPal.text
                            selectedTextColor: qgcPal.windowShade
                            selectionColor: qgcPal.text
                            font.pointSize: ScreenTools.defaultFontPointSize

                            background: Rectangle { color: qgcPal.window }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true

                        TextArea {
                            id: aiQuestion
                            Layout.fillWidth: true
                            Layout.preferredHeight: Math.min(ScreenTools.implicitTextFieldHeight * 3,
                                                             Math.max(ScreenTools.implicitTextFieldHeight,
                                                                      contentHeight + topPadding + bottomPadding))
                            placeholderText: qsTr("Ask about status or console output...")
                            enabled: aiController.configured && !aiController.busy
                            textFormat: TextEdit.PlainText
                            wrapMode: TextEdit.Wrap
                            selectByMouse: true
                            inputMethodHints: Qt.ImhMultiLine
                            color: qgcPal.text
                            selectedTextColor: qgcPal.windowShade
                            selectionColor: qgcPal.text
                            font.pointSize: ScreenTools.defaultFontPointSize
                            padding: ScreenTools.defaultFontPixelWidth * 0.75
                            activeFocusOnPress: true

                            background: Rectangle {
                                color: qgcPal.window
                                border.color: aiQuestion.activeFocus ? qgcPal.buttonHighlight : qgcPal.text
                                border.width: aiQuestion.activeFocus ? 2 : 1
                                radius: 2
                            }

                        }

                        QGCButton {
                            text: qsTr("Ask")
                            Layout.alignment: Qt.AlignBottom
                            visible: !aiController.busy
                            enabled: aiController.configured && aiQuestion.text.trim().length > 0
                            onClicked: askAI()
                        }

                        QGCButton {
                            text: qsTr("Cancel")
                            Layout.alignment: Qt.AlignBottom
                            visible: aiController.busy
                            enabled: aiController.busy
                            onClicked: aiController.cancel()
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillHeight: true
                Layout.preferredWidth: pageRoot._aiCollapsedHandleWidth
                Layout.preferredHeight: availableHeight
                visible: !ScreenTools.isMobile && pageRoot._aiPanelCollapsed
                color: qgcPal.windowShade
                border.color: qgcPal.buttonBorder
                border.width: 1

                ColumnLayout {
                    anchors.top: parent.top
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.topMargin: ScreenTools.defaultFontPixelWidth
                    spacing: ScreenTools.defaultFontPixelHeight * 0.5

                    QGCButton {
                        text: "<"
                        Layout.preferredWidth: ScreenTools.implicitButtonHeight
                        Layout.preferredHeight: ScreenTools.implicitButtonHeight
                        onClicked: pageRoot.expandAIPanel()
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        ToolTip.text: qsTr("Show AI Assistant")
                    }
                }
            }
        }
    } // Component
} // AnalyzePage
