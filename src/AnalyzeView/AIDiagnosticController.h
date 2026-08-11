/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QList>
#include <QtCore/QMetaObject>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QVariant>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtPositioning/QGeoCoordinate>

#include "CodexAppServerClient.h"
#include "PX4DiagnosticProvider.h"
#include "Vehicle.h"

class Fact;
class FactGroup;
class AIAssistantSettings;

class AIDiagnosticController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool configured READ configured NOTIFY configuredChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged)
    Q_PROPERTY(bool pendingActionAvailable READ pendingActionAvailable NOTIFY pendingActionChanged)
    Q_PROPERTY(QString pendingActionTitle READ pendingActionTitle NOTIFY pendingActionChanged)
    Q_PROPERTY(QString pendingActionDescription READ pendingActionDescription NOTIFY pendingActionChanged)
    Q_PROPERTY(QString pendingActionCommand READ pendingActionCommand NOTIFY pendingActionChanged)
    Q_PROPERTY(QString pendingActionRisk READ pendingActionRisk NOTIFY pendingActionChanged)
    Q_PROPERTY(bool chatGptSignedIn READ chatGptSignedIn NOTIFY chatGptAccountChanged)
    Q_PROPERTY(bool chatGptLoginInProgress READ chatGptLoginInProgress NOTIFY chatGptLoginChanged)
    Q_PROPERTY(QString chatGptStatusText READ chatGptStatusText NOTIFY chatGptStatusChanged)
    Q_PROPERTY(QString chatGptAccountEmail READ chatGptAccountEmail NOTIFY chatGptAccountChanged)
    Q_PROPERTY(QString chatGptPlanType READ chatGptPlanType NOTIFY chatGptAccountChanged)
    Q_PROPERTY(QString chatGptVerificationUrl READ chatGptVerificationUrl NOTIFY chatGptLoginChanged)
    Q_PROPERTY(QString chatGptUserCode READ chatGptUserCode NOTIFY chatGptLoginChanged)
    Q_PROPERTY(QVariantList chatGptModels READ chatGptModels NOTIFY chatGptModelsChanged)
    Q_PROPERTY(QStringList chatGptModelNames READ chatGptModelNames NOTIFY chatGptModelsChanged)
    Q_PROPERTY(int chatGptModelIndex READ chatGptModelIndex NOTIFY chatGptModelsChanged)
    Q_PROPERTY(bool activeVehicleAvailable READ activeVehicleAvailable NOTIFY activeVehicleChanged)
    Q_PROPERTY(bool activeVehicleSupported READ activeVehicleSupported NOTIFY activeVehicleChanged)
    Q_PROPERTY(QString activeVehicleStatusText READ activeVehicleStatusText NOTIFY activeVehicleChanged)

public:
    explicit AIDiagnosticController(QObject *parent = nullptr);
    ~AIDiagnosticController() override;

    bool busy() const { return _busy; }
    bool configured() const;
    QString errorText() const { return _errorText; }
    bool pendingActionAvailable() const { return _pendingActionAvailable; }
    QString pendingActionTitle() const { return _pendingActionTitle; }
    QString pendingActionDescription() const { return _pendingActionDescription; }
    QString pendingActionCommand() const { return _pendingActionCommand; }
    QString pendingActionRisk() const { return _pendingActionRisk; }
    bool chatGptSignedIn() const { return _chatGptSignedIn; }
    bool chatGptLoginInProgress() const { return _chatGptLoginInProgress; }
    QString chatGptStatusText() const { return _chatGptStatusText; }
    QString chatGptAccountEmail() const { return _chatGptAccountEmail; }
    QString chatGptPlanType() const { return _chatGptPlanType; }
    QString chatGptVerificationUrl() const { return _chatGptVerificationUrl; }
    QString chatGptUserCode() const { return _chatGptUserCode; }
    QVariantList chatGptModels() const { return _chatGptModels; }
    QStringList chatGptModelNames() const;
    int chatGptModelIndex() const { return _chatGptModelIndex; }
    bool activeVehicleAvailable() const;
    bool activeVehicleSupported() const;
    QString activeVehicleStatusText() const;

    Q_INVOKABLE void ask(const QString &question);
    Q_INVOKABLE void askWithEvidence(const QString &question, const QString &consoleText);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void clearConversation();
    Q_INVOKABLE void approvePendingAction();
    Q_INVOKABLE void rejectPendingAction();
    Q_INVOKABLE void startChatGptLogin();
    Q_INVOKABLE void cancelChatGptLogin();
    Q_INVOKABLE void signOutChatGpt();
    Q_INVOKABLE void openChatGptLoginPage();
    Q_INVOKABLE void copyChatGptUserCode();
    Q_INVOKABLE void selectChatGptModel(int index);

signals:
    void busyChanged();
    void configuredChanged();
    void errorTextChanged();
    void pendingActionChanged();
    void chatGptAccountChanged();
    void chatGptLoginChanged();
    void chatGptStatusChanged();
    void chatGptModelsChanged();
    void answerDelta(const QString &delta);
    void answerFinalized(const QString &answer);
    void answerReady(const QString &answer);
    void conversationCleared();
    void requestFailed(const QString &errorText);
    void activeVehicleChanged();

private slots:
    void _replyFinished();
    void _requestTimedOut();
    void _consoleCommandTimedOut();
    void _receiveConsoleData(uint8_t device, uint8_t flags, uint16_t timeout, uint32_t baudrate, const QByteArray &data);
    void _activeVehicleChanged(Vehicle *vehicle);

private:
    friend class AIDiagnosticControllerTest;

    enum ToolExecutionKind {
        ToolExecutionImmediate,
        ToolExecutionConsole,
        ToolExecutionRequestMessage,
        ToolExecutionSetMessageInterval
    };

    struct PendingConsoleToolCommand {
        QString toolCallId;
        QString toolName;
        QString command;
        QJsonObject arguments;
        ToolExecutionKind executionKind = ToolExecutionImmediate;
        int componentId = MAV_COMP_ID_AUTOPILOT1;
        int messageId = 0;
        int intervalUsec = 0;
        int ttlSeconds = 0;
    };

    struct AIToolCallbackData {
        QPointer<AIDiagnosticController> controller;
        QPointer<Vehicle> vehicle;
        PendingConsoleToolCommand toolCommand;
    };

    bool _chatGptSelected() const;
    void _askChatGptWithContext(const QString &question, const QString &consoleText);
    void _ensureCodexClientStarted();
    void _readChatGptAccount();
    void _loadChatGptModels();
    void _handleCodexNotification(const QString &method, const QJsonObject &params);
    void _startChatGptThread(const QString &question, const QString &consoleText);
    void _startChatGptTurn(const QString &threadId, const QString &question, const QString &consoleText);
    static QString _responseLanguagePolicy();
    QString _standardSystemPrompt() const;
    QString _chatGptPrompt(const QString &question, const QString &consoleText) const;
    void _setChatGptLoginInProgress(bool inProgress);
    void _setChatGptStatus(const QString &statusText);
    void _resetChatGptLoginFields();
    bool _checkTlsAvailable(const QUrl &url, QString *errorText) const;
    QString _tlsUnavailableText() const;
    QString _formatNetworkErrorText(QNetworkReply::NetworkError networkError, const QUrl &url, const QString &errorText, const QByteArray &payload = QByteArray(), int httpStatusCode = 0) const;
    QString _responseErrorText(const QByteArray &payload) const;
    bool _shouldRetryNetworkError(QNetworkReply::NetworkError networkError) const;
    QString _authorizationHeaderValue(AIAssistantSettings *settings) const;
    QByteArray _formData(const QList<QPair<QString, QString>> &fields) const;
    bool _postChatRequest(const QJsonArray &messages, bool includeTools, const QString &toolChoice = QString());
    QJsonArray _buildToolDefinitions() const;
    QJsonObject _buildVehicleSnapshot(Vehicle *vehicle) const;
    QJsonObject _buildRequestContext(Vehicle *vehicle, const QString &consoleText, bool *consoleTextTruncated = nullptr) const;
    QJsonObject _buildHealthAndArmingCheckReportJson(Vehicle *vehicle) const;
    QJsonObject _buildLinkStatusJson(Vehicle *vehicle) const;
    QJsonObject _factGroupToJson(const FactGroup *factGroup) const;
    QJsonObject _parameterToJson(const Fact *fact) const;
    QJsonObject _mavlinkMessageToJson(const mavlink_message_t &message) const;
    QString _trimConsoleContext(const QString &consoleText, bool *truncated) const;
    QList<PendingConsoleToolCommand> _automaticConsoleToolCommandsForQuestion(const QString &question) const;
    PendingConsoleToolCommand _makeSensorStatusToolCommand(const QString &toolCallId, const QString &sensorType) const;
    PendingConsoleToolCommand _makeParamToolCommand(const QString &toolCallId, const QString &paramName) const;
    QJsonObject _toolCallMessageForCommands(const QList<PendingConsoleToolCommand> &toolCommands) const;
    QJsonObject _toolCallObjectForCommand(const PendingConsoleToolCommand &toolCommand) const;
    void _startToolExecution(const QJsonArray &toolCalls);
    void _appendToolResult(const PendingConsoleToolCommand &toolCommand, const QJsonObject &result);
    bool _buildAIToolCommand(const QJsonObject &toolCall, PendingConsoleToolCommand *toolCommand, QJsonObject *immediateResult) const;
    QJsonObject _executeImmediateToolCommand(const PendingConsoleToolCommand &toolCommand) const;
    QString _consoleCommandForSensorStatus(const QString &sensorType) const;
    QString _normalizedSensorType(const QString &sensorType) const;
    bool _isSafePx4ParameterName(const QString &paramName) const;
    bool _isWhitelistedConsoleCommand(const QString &command) const;
    bool _isSafeMavlinkMessageId(int messageId) const;
    bool _validateVehicleForAITool(Vehicle *vehicle, const QString &toolName, QString *errorText) const;
    bool _toolRequiresUserApproval(const PendingConsoleToolCommand &toolCommand) const;
    void _setPendingApproval(const PendingConsoleToolCommand &toolCommand);
    void _clearPendingApproval();
    QJsonObject _toolResultObject(const QString &toolCallId, const QString &toolName, const QJsonObject &arguments, const QString &status, const QString &message, const QString &command = QString(), const QString &output = QString(), bool outputTruncated = false) const;
    void _executeNextConsoleToolCommand();
    void _executePendingMavlinkToolCommand(const PendingConsoleToolCommand &toolCommand);
    void _finishActiveConsoleToolCommand(const QString &status, const QString &message);
    void _finishActiveMavlinkToolCommand(const PendingConsoleToolCommand &toolCommand, const QJsonObject &result);
    void _finishToolExecution();
    void _clearConsoleToolExecution(bool sendClose);
    void _sendSerialData(const QByteArray &data, bool close = false);
    QString _cleanConsoleOutput(const QByteArray &output, bool *truncated) const;
    bool _answerContainsInternalToolMarkup(const QString &answer) const;
    static void _requestMessageResultHandler(void *resultHandlerData, MAV_RESULT commandResult, Vehicle::RequestMessageResultHandlerFailureCode_t failureCode, const mavlink_message_t &message);
    static void _mavCommandResultHandler(void *resultHandlerData, int compId, const mavlink_command_ack_t &ack, Vehicle::MavCmdResultFailureCode_t failureCode);
    void _clearPendingChatState();
    void _bindRequestVehicle(Vehicle *vehicle);
    void _unbindRequestVehicle();
    void _observeActiveVehicle(Vehicle *vehicle);
    void _appendConversationMessage(const QString &role, const QString &content);
    void _setBusy(bool busy);
    void _setErrorText(const QString &errorText);
    void _failRequest(const QString &errorText);
    void _clearReply(bool abortReply);

    QNetworkAccessManager _networkManager;
    CodexAppServerClient _codexClient;
    PX4DiagnosticProvider _px4Provider;
    QPointer<Vehicle> _requestVehicle;
    QNetworkReply *_reply = nullptr;
    QTimer _timeoutTimer;
    QTimer _consoleCommandTimer;
    QJsonArray _conversationHistory;
    QJsonArray _pendingMessages;
    QJsonArray _pendingToolResultMessages;
    QList<PendingConsoleToolCommand> _pendingConsoleToolCommands;
    PendingConsoleToolCommand _activeConsoleToolCommand;
    PendingConsoleToolCommand _activeMavlinkToolCommand;
    PendingConsoleToolCommand _pendingApprovalToolCommand;
    QMetaObject::Connection _consoleDataConnection;
    QMetaObject::Connection _requestVehicleDestroyedConnection;
    QMetaObject::Connection _requestVehicleCommunicationConnection;
    QList<QMetaObject::Connection> _activeVehicleStatusConnections;
    QByteArray _activeConsoleOutput;
    QString _errorText;
    QString _pendingActionTitle;
    QString _pendingActionDescription;
    QString _pendingActionCommand;
    QString _pendingActionRisk;
    QString _pendingQuestion;
    QString _chatGptStatusText;
    QString _chatGptAccountEmail;
    QString _chatGptPlanType;
    QString _chatGptVerificationUrl;
    QString _chatGptUserCode;
    QString _chatGptLoginId;
    QString _chatGptThreadId;
    QString _chatGptTurnId;
    QString _chatGptAnswer;
    QVariantList _chatGptModels;
    QString _pendingToolChoice;
    int _chatGptModelIndex = -1;
    int _remainingToolRounds = 0;
    int _remainingConsoleCommands = 0;
    int _remainingLowPrivilegeMavlinkCommands = 0;
    int _networkRetryCount = 0;
    quint64 _requestGeneration = 0;
    bool _pendingRequestIncludesTools = false;
    bool _internalToolMarkupRetryUsed = false;
    bool _automaticToolExecution = false;
    bool _activeConsoleOutputTruncated = false;
    bool _pendingActionAvailable = false;
    bool _busy = false;
    bool _chatGptSignedIn = false;
    bool _chatGptLoginInProgress = false;
    bool _chatGptLoginRequested = false;
    bool _requestUsesChatGpt = false;

    static constexpr int kRequestTimeoutMsec = 30000;
    static constexpr int kNetworkRetryDelayMsec = 1200;
    static constexpr int kConsoleCommandTimeoutMsec = 3000;
    static constexpr int kConsoleContextMaxChars = 12000;
    static constexpr int kConsoleCommandOutputMaxChars = 8192;
    static constexpr int kNetworkErrorBodyMaxChars = 2000;
    static constexpr int kConversationMessageLimit = 16;
    static constexpr int kMaxToolRounds = 1;
    static constexpr int kMaxConsoleCommandsPerQuestion = 3;
    static constexpr int kMaxLowPrivilegeMavlinkCommandsPerQuestion = 1;
    static constexpr int kMaxMavlinkMessageIntervalHz = 5;
    static constexpr int kMaxMavlinkMessageIntervalTtlSeconds = 30;
    static constexpr int kMaxNetworkRetryCount = 2;
    static constexpr int kAuthMethodApiKey = 0;
    static constexpr int kAuthMethodChatGpt = 1;
};
