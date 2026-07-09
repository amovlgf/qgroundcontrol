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
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QVariant>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtPositioning/QGeoCoordinate>

class Fact;
class FactGroup;
class AIConsoleSettings;
class Vehicle;

class MAVLinkConsoleAIController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool configured READ configured NOTIFY configuredChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged)
    Q_PROPERTY(bool oauthBusy READ oauthBusy NOTIFY oauthBusyChanged)
    Q_PROPERTY(bool oauthAuthorized READ oauthAuthorized NOTIFY oauthAuthorizedChanged)
    Q_PROPERTY(QString oauthStatusText READ oauthStatusText NOTIFY oauthStatusTextChanged)
    Q_PROPERTY(QString oauthUserCode READ oauthUserCode NOTIFY oauthAuthorizationChanged)
    Q_PROPERTY(QString oauthVerificationUri READ oauthVerificationUri NOTIFY oauthAuthorizationChanged)
    Q_PROPERTY(QString oauthVerificationUriComplete READ oauthVerificationUriComplete NOTIFY oauthAuthorizationChanged)
    Q_PROPERTY(QString oauthMessage READ oauthMessage NOTIFY oauthAuthorizationChanged)
    Q_PROPERTY(QString oauthExpiresAtText READ oauthExpiresAtText NOTIFY oauthAuthorizedChanged)

public:
    explicit MAVLinkConsoleAIController(QObject *parent = nullptr);
    ~MAVLinkConsoleAIController() override;

    bool busy() const { return _busy; }
    bool configured() const;
    QString errorText() const { return _errorText; }
    bool oauthBusy() const { return _oauthBusy; }
    bool oauthAuthorized() const;
    QString oauthStatusText() const { return _oauthStatusText; }
    QString oauthUserCode() const { return _oauthUserCode; }
    QString oauthVerificationUri() const { return _oauthVerificationUri; }
    QString oauthVerificationUriComplete() const { return _oauthVerificationUriComplete; }
    QString oauthMessage() const { return _oauthMessage; }
    QString oauthExpiresAtText() const;

    Q_INVOKABLE void ask(const QString &question);
    Q_INVOKABLE void askWithContext(const QString &question, const QString &consoleText);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void clearConversation();
    Q_INVOKABLE void startOAuthDeviceAuthorization();
    Q_INVOKABLE void cancelOAuthAuthorization();
    Q_INVOKABLE void clearOAuthTokens();

signals:
    void busyChanged();
    void configuredChanged();
    void errorTextChanged();
    void oauthBusyChanged();
    void oauthAuthorizedChanged();
    void oauthStatusTextChanged();
    void oauthAuthorizationChanged();
    void answerReady(const QString &answer);
    void conversationCleared();
    void requestFailed(const QString &errorText);

private slots:
    void _replyFinished();
    void _requestTimedOut();
    void _consoleCommandTimedOut();
    void _receiveConsoleData(uint8_t device, uint8_t flags, uint16_t timeout, uint32_t baudrate, const QByteArray &data);
    void _oauthDeviceAuthorizationFinished();
    void _oauthTokenPollFinished();
    void _pollOAuthToken();

private:
    struct PendingConsoleToolCommand {
        QString toolCallId;
        QString toolName;
        QString command;
        QJsonObject arguments;
    };

    bool _oauthSelected() const;
    bool _oauthTokenUsable() const;
    bool _checkTlsAvailable(const QUrl &url, QString *errorText) const;
    QString _tlsUnavailableText() const;
    QString _formatNetworkErrorText(QNetworkReply::NetworkError networkError, const QUrl &url, const QString &errorText, const QByteArray &payload = QByteArray(), int httpStatusCode = 0) const;
    QString _responseErrorText(const QByteArray &payload) const;
    bool _shouldRetryNetworkError(QNetworkReply::NetworkError networkError) const;
    QString _authorizationHeaderValue(AIConsoleSettings *settings) const;
    QByteArray _formData(const QList<QPair<QString, QString>> &fields) const;
    bool _postChatRequest(const QJsonArray &messages, bool includeTools, const QString &toolChoice = QString());
    QJsonArray _buildToolDefinitions() const;
    QJsonObject _buildVehicleSnapshot(Vehicle *vehicle) const;
    QJsonObject _coordinateToJson(const QGeoCoordinate &coordinate) const;
    QJsonObject _buildSysStatusSensorInfoJson(Vehicle *vehicle) const;
    QJsonObject _buildHealthAndArmingCheckReportJson(Vehicle *vehicle) const;
    QJsonObject _factGroupToJson(const FactGroup *factGroup) const;
    QJsonObject _factToJson(const Fact *fact) const;
    QJsonValue _variantToJson(const QVariant &value) const;
    QString _trimConsoleContext(const QString &consoleText, bool *truncated) const;
    QList<PendingConsoleToolCommand> _automaticConsoleToolCommandsForQuestion(const QString &question) const;
    PendingConsoleToolCommand _makeSensorStatusToolCommand(const QString &toolCallId, const QString &sensorType) const;
    PendingConsoleToolCommand _makeParamToolCommand(const QString &toolCallId, const QString &paramName) const;
    QJsonObject _toolCallMessageForCommands(const QList<PendingConsoleToolCommand> &toolCommands) const;
    QJsonObject _toolCallObjectForCommand(const PendingConsoleToolCommand &toolCommand) const;
    QString _firstPx4ParameterNameInQuestion(const QString &question) const;
    bool _questionContainsAny(const QString &normalizedQuestion, const QStringList &needles) const;
    void _startToolExecution(const QJsonArray &toolCalls);
    void _appendToolResult(const PendingConsoleToolCommand &toolCommand, const QJsonObject &result);
    bool _buildConsoleToolCommand(const QJsonObject &toolCall, PendingConsoleToolCommand *toolCommand, QJsonObject *immediateResult) const;
    QString _consoleCommandForSensorStatus(const QString &sensorType) const;
    QString _normalizedSensorType(const QString &sensorType) const;
    bool _isSafePx4ParameterName(const QString &paramName) const;
    bool _isWhitelistedConsoleCommand(const QString &command) const;
    QJsonObject _toolResultObject(const QString &toolCallId, const QString &toolName, const QJsonObject &arguments, const QString &status, const QString &message, const QString &command = QString(), const QString &output = QString(), bool outputTruncated = false) const;
    void _executeNextConsoleToolCommand();
    void _finishActiveConsoleToolCommand(const QString &status, const QString &message);
    void _finishToolExecution();
    void _clearConsoleToolExecution(bool sendClose);
    void _sendSerialData(const QByteArray &data, bool close = false);
    QString _cleanConsoleOutput(const QByteArray &output, bool *truncated) const;
    void _clearPendingChatState();
    void _appendConversationMessage(const QString &role, const QString &content);
    void _setBusy(bool busy);
    void _setErrorText(const QString &errorText);
    void _failRequest(const QString &errorText);
    void _clearReply(bool abortReply);
    void _setOAuthBusy(bool busy);
    void _setOAuthStatusText(const QString &statusText);
    void _failOAuth(const QString &errorText);
    void _clearOAuthReply(bool abortReply);
    void _clearOAuthAuthorizationFields();

    QNetworkAccessManager _networkManager;
    QNetworkReply *_reply = nullptr;
    QNetworkReply *_oauthReply = nullptr;
    QTimer _timeoutTimer;
    QTimer _consoleCommandTimer;
    QTimer _oauthPollTimer;
    QJsonArray _conversationHistory;
    QJsonArray _pendingMessages;
    QJsonArray _pendingToolResultMessages;
    QList<PendingConsoleToolCommand> _pendingConsoleToolCommands;
    PendingConsoleToolCommand _activeConsoleToolCommand;
    QMetaObject::Connection _consoleDataConnection;
    QByteArray _activeConsoleOutput;
    QString _errorText;
    QString _pendingQuestion;
    QString _oauthDeviceCode;
    QString _oauthUserCode;
    QString _oauthVerificationUri;
    QString _oauthVerificationUriComplete;
    QString _oauthMessage;
    QString _oauthStatusText;
    QString _pendingToolChoice;
    QDateTime _oauthDeviceCodeExpiresAtUtc;
    int _oauthPollIntervalMsec = 5000;
    int _remainingToolRounds = 0;
    int _remainingConsoleCommands = 0;
    int _networkRetryCount = 0;
    bool _pendingRequestIncludesTools = false;
    bool _automaticToolExecution = false;
    bool _activeConsoleOutputTruncated = false;
    bool _busy = false;
    bool _oauthBusy = false;

    static constexpr int kRequestTimeoutMsec = 30000;
    static constexpr int kNetworkRetryDelayMsec = 1200;
    static constexpr int kConsoleCommandTimeoutMsec = 3000;
    static constexpr int kConsoleContextMaxChars = 12000;
    static constexpr int kConsoleCommandOutputMaxChars = 8192;
    static constexpr int kNetworkErrorBodyMaxChars = 2000;
    static constexpr int kConversationMessageLimit = 16;
    static constexpr int kMaxToolRounds = 1;
    static constexpr int kMaxConsoleCommandsPerQuestion = 3;
    static constexpr int kMaxNetworkRetryCount = 2;
    static constexpr int kAuthMethodApiKey = 0;
    static constexpr int kAuthMethodOAuthDevice = 1;
};
