/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "MAVLinkConsoleAIController.h"
#include "AIConsoleSettings.h"
#include "Fact.h"
#include "FactGroup.h"
#include "HealthAndArmingCheckReport.h"
#include "MAVLinkProtocol.h"
#include "MultiVehicleManager.h"
#include "ParameterManager.h"
#include "QGCMAVLink.h"
#include "QmlObjectListModel.h"
#include "SettingsManager.h"
#include "Vehicle.h"
#include "VehicleLinkManager.h"

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QMetaType>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QStringList>
#include <QtCore/QUrlQuery>
#include <QtCore/QVariant>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QSslSocket>
#include <QtPositioning/QGeoCoordinate>

#include <cmath>

namespace {

QString mavResultText(MAV_RESULT result)
{
    return QGCMAVLink::mavResultToString(result);
}

QString requestMessageFailureText(Vehicle::RequestMessageResultHandlerFailureCode_t failureCode)
{
    switch (failureCode) {
    case Vehicle::RequestMessageNoFailure:
        return QStringLiteral("NoFailure");
    case Vehicle::RequestMessageFailureCommandError:
        return QStringLiteral("CommandError");
    case Vehicle::RequestMessageFailureCommandNotAcked:
        return QStringLiteral("CommandNotAcked");
    case Vehicle::RequestMessageFailureMessageNotReceived:
        return QStringLiteral("MessageNotReceived");
    case Vehicle::RequestMessageFailureDuplicateCommand:
        return QStringLiteral("DuplicateCommand");
    }

    return QStringLiteral("UnknownFailure");
}

QString mavCommandFailureText(Vehicle::MavCmdResultFailureCode_t failureCode)
{
    switch (failureCode) {
    case Vehicle::MavCmdResultCommandResultOnly:
        return QStringLiteral("CommandResultOnly");
    case Vehicle::MavCmdResultFailureNoResponseToCommand:
        return QStringLiteral("NoResponseToCommand");
    case Vehicle::MavCmdResultFailureDuplicateCommand:
        return QStringLiteral("DuplicateCommand");
    }

    return QStringLiteral("UnknownFailure");
}

} // namespace

MAVLinkConsoleAIController::MAVLinkConsoleAIController(QObject *parent)
    : QObject(parent)
    , _networkManager(this)
{
    _timeoutTimer.setSingleShot(true);
    _timeoutTimer.setInterval(kRequestTimeoutMsec);
    (void) connect(&_timeoutTimer, &QTimer::timeout, this, &MAVLinkConsoleAIController::_requestTimedOut);
    _consoleCommandTimer.setSingleShot(true);
    _consoleCommandTimer.setInterval(kConsoleCommandTimeoutMsec);
    (void) connect(&_consoleCommandTimer, &QTimer::timeout, this, &MAVLinkConsoleAIController::_consoleCommandTimedOut);
    _oauthPollTimer.setSingleShot(true);
    (void) connect(&_oauthPollTimer, &QTimer::timeout, this, &MAVLinkConsoleAIController::_pollOAuthToken);

    if (AIConsoleSettings *settings = SettingsManager::instance()->aiConsoleSettings()) {
        (void) connect(settings->authMethod(), &Fact::rawValueChanged, this, [this] { emit configuredChanged(); });
        (void) connect(settings->endpointUrl(), &Fact::rawValueChanged, this, [this] { emit configuredChanged(); });
        (void) connect(settings->modelName(), &Fact::rawValueChanged, this, [this] { emit configuredChanged(); });
        (void) connect(settings->oauthAccessToken(), &Fact::rawValueChanged, this, [this] {
            emit configuredChanged();
            emit oauthAuthorizedChanged();
        });
        (void) connect(settings->oauthTokenExpiresAtUtc(), &Fact::rawValueChanged, this, [this] {
            emit configuredChanged();
            emit oauthAuthorizedChanged();
        });
    }

    (void) connect(MultiVehicleManager::instance(), &MultiVehicleManager::activeVehicleChanged, this, [this] {
        clearConversation();
    });
}

MAVLinkConsoleAIController::~MAVLinkConsoleAIController()
{
    _clearReply(true);
    _clearOAuthReply(true);
}

bool MAVLinkConsoleAIController::configured() const
{
    AIConsoleSettings *settings = SettingsManager::instance()->aiConsoleSettings();
    const bool endpointConfigured = settings
        && !settings->endpointUrl()->rawValueString().trimmed().isEmpty()
        && !settings->modelName()->rawValueString().trimmed().isEmpty();

    if (!endpointConfigured) {
        return false;
    }

    return !_oauthSelected() || _oauthTokenUsable();
}

bool MAVLinkConsoleAIController::oauthAuthorized() const
{
    return _oauthTokenUsable();
}

QString MAVLinkConsoleAIController::oauthExpiresAtText() const
{
    AIConsoleSettings *settings = SettingsManager::instance()->aiConsoleSettings();
    if (!settings) {
        return QString();
    }

    const QString expiresAtText = settings->oauthTokenExpiresAtUtc()->rawValueString().trimmed();
    if (expiresAtText.isEmpty()) {
        return _oauthTokenUsable() ? tr("No expiration reported") : QString();
    }

    bool ok = false;
    const qint64 expiresAtSecs = expiresAtText.toLongLong(&ok);
    if (!ok) {
        return QString();
    }

    return QDateTime::fromSecsSinceEpoch(expiresAtSecs, Qt::UTC).toLocalTime().toString(Qt::ISODate);
}

void MAVLinkConsoleAIController::ask(const QString &question)
{
    askWithContext(question, QString());
}

void MAVLinkConsoleAIController::askWithContext(const QString &question, const QString &consoleText)
{
    const QString trimmedQuestion = question.trimmed();
    if (trimmedQuestion.isEmpty()) {
        return;
    }

    if (_busy) {
        _failRequest(tr("An AI request is already running."));
        return;
    }

    AIConsoleSettings *settings = SettingsManager::instance()->aiConsoleSettings();
    if (!settings) {
        _failRequest(tr("AI console settings are not available."));
        return;
    }

    const QString endpointUrl = settings->endpointUrl()->rawValueString().trimmed();
    const QString modelName = settings->modelName()->rawValueString().trimmed();
    const QString authorizationHeaderValue = _authorizationHeaderValue(settings);
    if (endpointUrl.isEmpty() || modelName.isEmpty()) {
        _failRequest(tr("Configure an AI endpoint URL and model first."));
        return;
    }
    if (_oauthSelected() && authorizationHeaderValue.isEmpty()) {
        _failRequest(tr("Authorize OAuth in AI Assistant settings first."));
        return;
    }

    const QUrl url = QUrl::fromUserInput(endpointUrl);
    if (!url.isValid()) {
        _failRequest(tr("The AI endpoint URL is invalid."));
        return;
    }
    QString tlsErrorText;
    if (!_checkTlsAvailable(url, &tlsErrorText)) {
        _failRequest(tlsErrorText);
        return;
    }

    _setErrorText(QString());

    Vehicle *vehicle = MultiVehicleManager::instance()->activeVehicle();
    const QJsonDocument snapshotDocument(_buildVehicleSnapshot(vehicle));
    bool consoleContextTruncated = false;
    const QString consoleContext = _trimConsoleContext(consoleText, &consoleContextTruncated);
    const QString consoleContextNote = consoleContextTruncated
        ? QStringLiteral(" (tail, truncated to the last %1 characters)").arg(kConsoleContextMaxChars)
        : QString();
    const QString consoleContextText = consoleContext.isEmpty()
        ? tr("No MAVLink Console output was provided.")
        : consoleContext;

    const QString systemPrompt = tr(
        "You are a PX4-focused QGroundControl vehicle assistant with a hard local tool registry. "
        "Answer in the user's language. Use only the provided active vehicle JSON snapshot, recent MAVLink Console output, tool results, and chat history. "
        "Do not invent values. If a value is missing, stale, unavailable, or a tool fails, say so. "
        "Prioritize QGroundControl telemetry and PX4 health/arming data over generic knowledge. "
        "Clearly distinguish confirmed facts from likely causes. "
        "Never control, arm, disarm, take off, land, change modes, move the vehicle, calibrate sensors, reboot, set or reset parameters, disable checks, run actuator tests, modify missions/geofences/rally points, or execute arbitrary shell/MAVLink commands. "
        "Use the provided structured tools when they help. Read-only vehicle information tools execute automatically. "
        "Low-privilege MAVLink data tools are limited to REQUEST_MESSAGE and temporary SET_MESSAGE_INTERVAL for whitelisted telemetry messages; QGroundControl will ask the user for confirmation before sending them. "
        "Do not ask for low-privilege MAVLink tools unless the user is asking for data that is missing from the snapshot or needs a fresh message. "
        "For natural-language diagnostics, use the provided read-only PX4 shell tools only when they are needed. These tools are internally restricted to PX4 diagnostic queries. "
        "Some common read-only diagnostics may already be run automatically by QGroundControl before you answer; use those tool results as primary evidence. "
        "Never print tool-call markup, DSML, XML-like tool syntax, JSON tool payloads, tool_calls blocks, or internal tool metadata to the user. If tool calling is unavailable, answer directly from the provided context. "
        "If asked for control or unsafe actions, refuse briefly and suggest safe diagnosis. "
        "When reporting a tool result, mention the command/data source if relevant. "
        "Keep answers concise and practical.");

    const QString userContent = QStringLiteral(
        "Question:\n%1\n\n"
        "Current QGroundControl vehicle status JSON:\n%2\n\n"
        "Recent MAVLink Console output%3:\n%4")
        .arg(trimmedQuestion,
             QString::fromUtf8(snapshotDocument.toJson(QJsonDocument::Compact)),
             consoleContextNote,
             consoleContextText);

    QJsonArray messages;
    messages.append(QJsonObject{
        { QStringLiteral("role"), QStringLiteral("system") },
        { QStringLiteral("content"), systemPrompt }
    });
    for (const QJsonValue &messageValue : _conversationHistory) {
        messages.append(messageValue);
    }
    messages.append(QJsonObject{
        { QStringLiteral("role"), QStringLiteral("user") },
        { QStringLiteral("content"), userContent }
    });

    _clearPendingChatState();
    _pendingMessages = messages;
    _remainingToolRounds = kMaxToolRounds;
    _remainingConsoleCommands = kMaxConsoleCommandsPerQuestion;
    _remainingLowPrivilegeMavlinkCommands = kMaxLowPrivilegeMavlinkCommandsPerQuestion;
    _networkRetryCount = 0;
    _internalToolMarkupRetryUsed = false;
    _pendingQuestion = trimmedQuestion;
    _setBusy(true);

    const QList<PendingConsoleToolCommand> automaticToolCommands = _automaticConsoleToolCommandsForQuestion(trimmedQuestion);
    if (!automaticToolCommands.isEmpty()) {
        _pendingToolResultMessages = QJsonArray();
        _automaticToolExecution = true;
        for (const PendingConsoleToolCommand &toolCommand : automaticToolCommands) {
            if (_remainingConsoleCommands <= 0) {
                break;
            }
            _pendingConsoleToolCommands.append(toolCommand);
            _remainingConsoleCommands--;
        }
        _executeNextConsoleToolCommand();
        return;
    }

    if (!_postChatRequest(_pendingMessages, true)) {
        _clearPendingChatState();
        _setBusy(false);
    }
}

void MAVLinkConsoleAIController::cancel()
{
    if (!_reply && !_consoleCommandTimer.isActive() && _pendingConsoleToolCommands.isEmpty() && !_pendingActionAvailable) {
        return;
    }

    _clearReply(true);
    _clearConsoleToolExecution(true);
    _clearPendingChatState();
    _setBusy(false);
    _pendingQuestion.clear();
    _failRequest(tr("AI request canceled."));
}

void MAVLinkConsoleAIController::clearConversation()
{
    _clearConsoleToolExecution(true);
    _clearPendingChatState();
    _conversationHistory = QJsonArray();
    _pendingQuestion.clear();
    _setErrorText(QString());
    emit conversationCleared();
}

void MAVLinkConsoleAIController::approvePendingAction()
{
    if (!_pendingActionAvailable) {
        return;
    }

    const PendingConsoleToolCommand toolCommand = _pendingApprovalToolCommand;
    _clearPendingApproval();
    _executePendingMavlinkToolCommand(toolCommand);
}

void MAVLinkConsoleAIController::rejectPendingAction()
{
    if (!_pendingActionAvailable) {
        return;
    }

    const PendingConsoleToolCommand toolCommand = _pendingApprovalToolCommand;
    _clearPendingApproval();
    _appendToolResult(toolCommand, _toolResultObject(
        toolCommand.toolCallId,
        toolCommand.toolName,
        toolCommand.arguments,
        QStringLiteral("rejected"),
        tr("The user rejected the low-privilege MAVLink action before it was sent."),
        toolCommand.command));
    _finishToolExecution();
}

void MAVLinkConsoleAIController::startOAuthDeviceAuthorization()
{
    if (_oauthBusy) {
        _setOAuthStatusText(tr("OAuth authorization is already running."));
        return;
    }

    AIConsoleSettings *settings = SettingsManager::instance()->aiConsoleSettings();
    if (!settings) {
        _failOAuth(tr("AI console settings are not available."));
        return;
    }

    const QString deviceAuthorizationUrl = settings->oauthDeviceAuthorizationUrl()->rawValueString().trimmed();
    const QString tokenUrl = settings->oauthTokenUrl()->rawValueString().trimmed();
    const QString clientId = settings->oauthClientId()->rawValueString().trimmed();
    const QString scope = settings->oauthScope()->rawValueString().trimmed();
    if (deviceAuthorizationUrl.isEmpty() || tokenUrl.isEmpty() || clientId.isEmpty()) {
        _failOAuth(tr("Configure OAuth device authorization URL, token URL, and client ID first."));
        return;
    }

    const QUrl url = QUrl::fromUserInput(deviceAuthorizationUrl);
    if (!url.isValid()) {
        _failOAuth(tr("The OAuth device authorization URL is invalid."));
        return;
    }
    QString tlsErrorText;
    if (!_checkTlsAvailable(url, &tlsErrorText)) {
        _failOAuth(tlsErrorText);
        return;
    }

    _clearOAuthAuthorizationFields();
    _setOAuthBusy(true);
    _setOAuthStatusText(tr("Requesting OAuth user code..."));

    QList<QPair<QString, QString>> fields{
        { QStringLiteral("client_id"), clientId }
    };
    if (!scope.isEmpty()) {
        fields.append({ QStringLiteral("scope"), scope });
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    request.setRawHeader(QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json"));
    request.setRawHeader(QByteArrayLiteral("User-Agent"), QByteArrayLiteral("QGroundControl-MAVLinkConsoleAI"));

    _oauthReply = _networkManager.post(request, _formData(fields));
    (void) connect(_oauthReply, &QNetworkReply::finished, this, &MAVLinkConsoleAIController::_oauthDeviceAuthorizationFinished);
}

void MAVLinkConsoleAIController::cancelOAuthAuthorization()
{
    if (!_oauthBusy && !_oauthReply && !_oauthPollTimer.isActive()) {
        return;
    }

    _oauthPollTimer.stop();
    _clearOAuthReply(true);
    _setOAuthBusy(false);
    _setOAuthStatusText(tr("OAuth authorization canceled."));
}

void MAVLinkConsoleAIController::clearOAuthTokens()
{
    AIConsoleSettings *settings = SettingsManager::instance()->aiConsoleSettings();
    if (!settings) {
        return;
    }

    settings->oauthAccessToken()->setRawValue(QString());
    settings->oauthRefreshToken()->setRawValue(QString());
    settings->oauthTokenExpiresAtUtc()->setRawValue(QString());
    _setOAuthStatusText(tr("OAuth tokens cleared."));
    emit configuredChanged();
    emit oauthAuthorizedChanged();
}

bool MAVLinkConsoleAIController::_postChatRequest(const QJsonArray &messages, bool includeTools, const QString &toolChoice)
{
    AIConsoleSettings *settings = SettingsManager::instance()->aiConsoleSettings();
    if (!settings) {
        _failRequest(tr("AI console settings are not available."));
        return false;
    }

    const QString endpointUrl = settings->endpointUrl()->rawValueString().trimmed();
    const QString modelName = settings->modelName()->rawValueString().trimmed();
    const QString authorizationHeaderValue = _authorizationHeaderValue(settings);
    if (endpointUrl.isEmpty() || modelName.isEmpty()) {
        _failRequest(tr("Configure an AI endpoint URL and model first."));
        return false;
    }
    if (_oauthSelected() && authorizationHeaderValue.isEmpty()) {
        _failRequest(tr("Authorize OAuth in AI Assistant settings first."));
        return false;
    }

    const QUrl url = QUrl::fromUserInput(endpointUrl);
    if (!url.isValid()) {
        _failRequest(tr("The AI endpoint URL is invalid."));
        return false;
    }
    QString tlsErrorText;
    if (!_checkTlsAvailable(url, &tlsErrorText)) {
        _failRequest(tlsErrorText);
        return false;
    }

    QJsonObject requestObject{
        { QStringLiteral("model"), modelName },
        { QStringLiteral("messages"), messages },
        { QStringLiteral("temperature"), 0.2 },
        { QStringLiteral("stream"), false }
    };

    if (includeTools) {
        requestObject.insert(QStringLiteral("tools"), _buildToolDefinitions());
        requestObject.insert(QStringLiteral("tool_choice"), toolChoice.isEmpty() ? QStringLiteral("auto") : toolChoice);
    }

    _pendingRequestIncludesTools = includeTools;
    _pendingToolChoice = includeTools ? (toolChoice.isEmpty() ? QStringLiteral("auto") : toolChoice) : QString();

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader(QByteArrayLiteral("User-Agent"), QByteArrayLiteral("QGroundControl-MAVLinkConsoleAI"));
    if (!authorizationHeaderValue.isEmpty()) {
        request.setRawHeader(QByteArrayLiteral("Authorization"), authorizationHeaderValue.toUtf8());
    }

    _reply = _networkManager.post(request, QJsonDocument(requestObject).toJson(QJsonDocument::Compact));
    (void) connect(_reply, &QNetworkReply::finished, this, &MAVLinkConsoleAIController::_replyFinished);
    _timeoutTimer.start();
    return true;
}

QJsonArray MAVLinkConsoleAIController::_buildToolDefinitions() const
{
    const QJsonObject emptyParameters{
        { QStringLiteral("type"), QStringLiteral("object") },
        { QStringLiteral("properties"), QJsonObject() },
        { QStringLiteral("additionalProperties"), false }
    };

    const QJsonObject factGroupParameters{
        { QStringLiteral("type"), QStringLiteral("object") },
        { QStringLiteral("properties"), QJsonObject{
            { QStringLiteral("group_name"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("string") },
                { QStringLiteral("description"), QStringLiteral("FactGroup name to return, for example vehicle, gps, battery0, localPosition, estimatorStatus, distanceSensors, escStatus.") }
            } }
        } },
        { QStringLiteral("required"), QJsonArray{ QStringLiteral("group_name") } },
        { QStringLiteral("additionalProperties"), false }
    };

    const QJsonObject parameterReadParameters{
        { QStringLiteral("type"), QStringLiteral("object") },
        { QStringLiteral("properties"), QJsonObject{
            { QStringLiteral("param_name"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("string") },
                { QStringLiteral("description"), QStringLiteral("Exact PX4 parameter id to read, such as COM_ARM_WO_GPS.") }
            } },
            { QStringLiteral("component_id"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("integer") },
                { QStringLiteral("description"), QStringLiteral("Optional MAVLink component id. Omit or use -1 for the default parameter component.") }
            } }
        } },
        { QStringLiteral("required"), QJsonArray{ QStringLiteral("param_name") } },
        { QStringLiteral("additionalProperties"), false }
    };

    const QJsonObject parameterSearchParameters{
        { QStringLiteral("type"), QStringLiteral("object") },
        { QStringLiteral("properties"), QJsonObject{
            { QStringLiteral("query"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("string") },
                { QStringLiteral("description"), QStringLiteral("Case-insensitive parameter name text to search for. Use a short prefix or keyword.") }
            } },
            { QStringLiteral("limit"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("integer") },
                { QStringLiteral("description"), QStringLiteral("Optional result limit from 1 to 25. Defaults to 10.") }
            } },
            { QStringLiteral("component_id"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("integer") },
                { QStringLiteral("description"), QStringLiteral("Optional MAVLink component id. Omit or use -1 for the default parameter component.") }
            } }
        } },
        { QStringLiteral("required"), QJsonArray{ QStringLiteral("query") } },
        { QStringLiteral("additionalProperties"), false }
    };

    const QJsonObject requestMessageParameters{
        { QStringLiteral("type"), QStringLiteral("object") },
        { QStringLiteral("properties"), QJsonObject{
            { QStringLiteral("message_id"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("integer") },
                { QStringLiteral("description"), QStringLiteral("Whitelisted MAVLink message id to request once, such as AUTOPILOT_VERSION=148, SYS_STATUS=1, GPS_RAW_INT=24, BATTERY_STATUS=147.") }
            } },
            { QStringLiteral("component_id"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("integer") },
                { QStringLiteral("description"), QStringLiteral("Optional target component id. Defaults to the vehicle default component.") }
            } }
        } },
        { QStringLiteral("required"), QJsonArray{ QStringLiteral("message_id") } },
        { QStringLiteral("additionalProperties"), false }
    };

    const QJsonObject messageIntervalParameters{
        { QStringLiteral("type"), QStringLiteral("object") },
        { QStringLiteral("properties"), QJsonObject{
            { QStringLiteral("message_id"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("integer") },
                { QStringLiteral("description"), QStringLiteral("Whitelisted MAVLink telemetry message id whose stream rate should be temporarily requested.") }
            } },
            { QStringLiteral("rate_hz"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("number") },
                { QStringLiteral("description"), QStringLiteral("Temporary rate in Hz. Must be greater than 0 and no more than 5 Hz.") }
            } },
            { QStringLiteral("ttl_seconds"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("integer") },
                { QStringLiteral("description"), QStringLiteral("How long to keep the requested stream rate before restoring default, from 1 to 30 seconds. Defaults to 10.") }
            } },
            { QStringLiteral("component_id"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("integer") },
                { QStringLiteral("description"), QStringLiteral("Optional target component id. Defaults to the vehicle default component.") }
            } }
        } },
        { QStringLiteral("required"), QJsonArray{ QStringLiteral("message_id"), QStringLiteral("rate_hz") } },
        { QStringLiteral("additionalProperties"), false }
    };

    const QJsonObject sensorParameters{
        { QStringLiteral("type"), QStringLiteral("object") },
        { QStringLiteral("properties"), QJsonObject{
            { QStringLiteral("sensor_type"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("string") },
                { QStringLiteral("description"), QStringLiteral("PX4 read-only diagnostic target to query.") },
                { QStringLiteral("enum"), QJsonArray{
                    QStringLiteral("all"),
                    QStringLiteral("accel"),
                    QStringLiteral("gyro"),
                    QStringLiteral("mag"),
                    QStringLiteral("baro"),
                    QStringLiteral("gps"),
                    QStringLiteral("battery"),
                    QStringLiteral("estimator"),
                    QStringLiteral("local_position"),
                    QStringLiteral("global_position"),
                    QStringLiteral("distance_sensor"),
                    QStringLiteral("optical_flow"),
                    QStringLiteral("commander"),
                    QStringLiteral("mavlink"),
                    QStringLiteral("version")
                } }
            } }
        } },
        { QStringLiteral("required"), QJsonArray{ QStringLiteral("sensor_type") } },
        { QStringLiteral("additionalProperties"), false }
    };

    return QJsonArray{
        QJsonObject{
            { QStringLiteral("type"), QStringLiteral("function") },
            { QStringLiteral("function"), QJsonObject{
                { QStringLiteral("name"), QStringLiteral("get_vehicle_status") },
                { QStringLiteral("description"), QStringLiteral("Return the current active vehicle status snapshot from QGroundControl telemetry and FactGroups.") },
                { QStringLiteral("parameters"), emptyParameters }
            } }
        },
        QJsonObject{
            { QStringLiteral("type"), QStringLiteral("function") },
            { QStringLiteral("function"), QJsonObject{
                { QStringLiteral("name"), QStringLiteral("get_fact_group") },
                { QStringLiteral("description"), QStringLiteral("Return one QGroundControl FactGroup by name. Use this for structured telemetry such as gps, battery0, localPosition, estimatorStatus, or distanceSensors.") },
                { QStringLiteral("parameters"), factGroupParameters }
            } }
        },
        QJsonObject{
            { QStringLiteral("type"), QStringLiteral("function") },
            { QStringLiteral("function"), QJsonObject{
                { QStringLiteral("name"), QStringLiteral("get_parameter") },
                { QStringLiteral("description"), QStringLiteral("Read one already-loaded PX4 parameter from QGroundControl's ParameterManager. This never writes parameters.") },
                { QStringLiteral("parameters"), parameterReadParameters }
            } }
        },
        QJsonObject{
            { QStringLiteral("type"), QStringLiteral("function") },
            { QStringLiteral("function"), QJsonObject{
                { QStringLiteral("name"), QStringLiteral("search_parameters") },
                { QStringLiteral("description"), QStringLiteral("Search already-loaded PX4 parameter names and return matching values and descriptions. This never writes parameters.") },
                { QStringLiteral("parameters"), parameterSearchParameters }
            } }
        },
        QJsonObject{
            { QStringLiteral("type"), QStringLiteral("function") },
            { QStringLiteral("function"), QJsonObject{
                { QStringLiteral("name"), QStringLiteral("get_health_report") },
                { QStringLiteral("description"), QStringLiteral("Return the PX4 health and arming check report currently known to QGroundControl.") },
                { QStringLiteral("parameters"), emptyParameters }
            } }
        },
        QJsonObject{
            { QStringLiteral("type"), QStringLiteral("function") },
            { QStringLiteral("function"), QJsonObject{
                { QStringLiteral("name"), QStringLiteral("get_link_status") },
                { QStringLiteral("description"), QStringLiteral("Return MAVLink link, communication-loss, and packet loss status for the active vehicle.") },
                { QStringLiteral("parameters"), emptyParameters }
            } }
        },
        QJsonObject{
            { QStringLiteral("type"), QStringLiteral("function") },
            { QStringLiteral("function"), QJsonObject{
                { QStringLiteral("name"), QStringLiteral("request_mavlink_message") },
                { QStringLiteral("description"), QStringLiteral("Ask QGroundControl to request one whitelisted MAVLink data/status message with MAV_CMD_REQUEST_MESSAGE. Requires user confirmation before sending.") },
                { QStringLiteral("parameters"), requestMessageParameters }
            } }
        },
        QJsonObject{
            { QStringLiteral("type"), QStringLiteral("function") },
            { QStringLiteral("function"), QJsonObject{
                { QStringLiteral("name"), QStringLiteral("set_message_interval") },
                { QStringLiteral("description"), QStringLiteral("Temporarily request a whitelisted MAVLink telemetry message stream rate with MAV_CMD_SET_MESSAGE_INTERVAL, then restore default after the TTL. Requires user confirmation before sending.") },
                { QStringLiteral("parameters"), messageIntervalParameters }
            } }
        },
        QJsonObject{
            { QStringLiteral("type"), QStringLiteral("function") },
            { QStringLiteral("function"), QJsonObject{
                { QStringLiteral("name"), QStringLiteral("query_sensor_status") },
                { QStringLiteral("description"), QStringLiteral("Run one restricted PX4 read-only diagnostic query for a sensor, estimator, commander, MAVLink, battery, position, or version status.") },
                { QStringLiteral("parameters"), sensorParameters }
            } }
        },
        QJsonObject{
            { QStringLiteral("type"), QStringLiteral("function") },
            { QStringLiteral("function"), QJsonObject{
                { QStringLiteral("name"), QStringLiteral("query_px4_param") },
                { QStringLiteral("description"), QStringLiteral("Read one PX4 parameter using a restricted param show command. This never changes parameters.") },
                { QStringLiteral("parameters"), parameterReadParameters }
            } }
        }
    };
}

void MAVLinkConsoleAIController::_replyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply || (reply != _reply)) {
        if (reply) {
            reply->deleteLater();
        }
        return;
    }

    _timeoutTimer.stop();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QUrl replyUrl = reply->url();
    const int httpStatusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray payload = reply->readAll();
    const QString networkErrorText = _formatNetworkErrorText(networkError, replyUrl, reply->errorString(), payload, httpStatusCode);
    _clearReply(false);

    if (networkError != QNetworkReply::NoError) {
        if (_shouldRetryNetworkError(networkError) && (_networkRetryCount < kMaxNetworkRetryCount)) {
            _networkRetryCount++;
            _setErrorText(tr("AI network request failed. Retrying %1/%2...")
                              .arg(_networkRetryCount)
                              .arg(kMaxNetworkRetryCount));
            QTimer::singleShot(kNetworkRetryDelayMsec, this, [this] {
                if (_busy && !_reply && !_pendingMessages.isEmpty()) {
                    (void) _postChatRequest(_pendingMessages, _pendingRequestIncludesTools, _pendingToolChoice);
                }
            });
            return;
        }

        _pendingQuestion.clear();
        _clearPendingChatState();
        _setBusy(false);
        _failRequest(networkErrorText);
        return;
    }

    QJsonParseError parseError{};
    const QJsonDocument responseDocument = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !responseDocument.isObject()) {
        _pendingQuestion.clear();
        _clearPendingChatState();
        _setBusy(false);
        _failRequest(tr("AI response was not valid JSON."));
        return;
    }

    const QJsonArray choices = responseDocument.object().value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        _pendingQuestion.clear();
        _clearPendingChatState();
        _setBusy(false);
        _failRequest(tr("AI response did not contain any choices."));
        return;
    }

    const QJsonObject messageObject = choices.at(0).toObject().value(QStringLiteral("message")).toObject();
    const QString answer = messageObject.value(QStringLiteral("content")).toString().trimmed();
    const QJsonArray toolCalls = messageObject.value(QStringLiteral("tool_calls")).toArray();

    if (!toolCalls.isEmpty() && (_remainingToolRounds > 0)) {
        _remainingToolRounds--;
        QJsonObject assistantToolMessage{
            { QStringLiteral("role"), QStringLiteral("assistant") },
            { QStringLiteral("tool_calls"), toolCalls }
        };
        if (!answer.isEmpty()) {
            assistantToolMessage.insert(QStringLiteral("content"), answer);
        } else {
            assistantToolMessage.insert(QStringLiteral("content"), QJsonValue());
        }
        _pendingMessages.append(assistantToolMessage);
        _startToolExecution(toolCalls);
        return;
    }

    if (answer.isEmpty()) {
        _pendingQuestion.clear();
        _clearPendingChatState();
        _setBusy(false);
        _failRequest(tr("AI response did not contain an answer."));
        return;
    }

    if (_answerContainsInternalToolMarkup(answer)) {
        if (!_internalToolMarkupRetryUsed) {
            _internalToolMarkupRetryUsed = true;
            _networkRetryCount = 0;
            _setErrorText(tr("AI returned internal tool-call text. Retrying without tools..."));
            if (!_postChatRequest(_pendingMessages, false)) {
                _pendingQuestion.clear();
                _clearPendingChatState();
                _setBusy(false);
            }
            return;
        }

        _pendingQuestion.clear();
        _clearPendingChatState();
        _setBusy(false);
        _failRequest(tr("The AI endpoint returned internal tool-call markup instead of a user-facing answer. This model or endpoint may not support structured tool calls."));
        return;
    }

    if (!_pendingQuestion.isEmpty()) {
        _appendConversationMessage(QStringLiteral("user"), _pendingQuestion);
        _appendConversationMessage(QStringLiteral("assistant"), answer);
        _pendingQuestion.clear();
    }

    _clearPendingChatState();
    _setBusy(false);
    emit answerReady(answer);
}

void MAVLinkConsoleAIController::_requestTimedOut()
{
    if (!_reply) {
        return;
    }

    _clearReply(true);
    _clearConsoleToolExecution(true);
    _clearPendingChatState();
    _setBusy(false);
    _pendingQuestion.clear();
    _failRequest(tr("AI request timed out."));
}

QList<MAVLinkConsoleAIController::PendingConsoleToolCommand> MAVLinkConsoleAIController::_automaticConsoleToolCommandsForQuestion(const QString &question) const
{
    QList<PendingConsoleToolCommand> toolCommands;
    QStringList addedCommands;
    int nextToolCallId = 1;

    auto appendToolCommand = [&](const PendingConsoleToolCommand &toolCommand) {
        if (toolCommand.command.isEmpty() || addedCommands.contains(toolCommand.command) || toolCommands.size() >= kMaxConsoleCommandsPerQuestion) {
            return;
        }
        if (!_isWhitelistedConsoleCommand(toolCommand.command)) {
            return;
        }
        toolCommands.append(toolCommand);
        addedCommands.append(toolCommand.command);
    };

    auto appendSensorStatus = [&](const QString &sensorType) {
        appendToolCommand(_makeSensorStatusToolCommand(QStringLiteral("auto_call_%1").arg(nextToolCallId++), sensorType));
    };

    auto appendParam = [&](const QString &paramName) {
        appendToolCommand(_makeParamToolCommand(QStringLiteral("auto_call_%1").arg(nextToolCallId++), paramName));
    };

    const QString normalizedQuestion = question.toLower();

    const QString paramName = _firstPx4ParameterNameInQuestion(question);
    if (!paramName.isEmpty() && _questionContainsAny(normalizedQuestion, {
            QStringLiteral("param"),
            QStringLiteral("parameter"),
            QStringLiteral("参数"),
            QStringLiteral("当前值"),
            QStringLiteral("是多少"),
            QStringLiteral("查询"),
            QStringLiteral("查看"),
            QStringLiteral("show")
        })) {
        appendParam(paramName);
    }

    if (_questionContainsAny(normalizedQuestion, {
            QStringLiteral("版本"),
            QStringLiteral("固件"),
            QStringLiteral("firmware"),
            QStringLiteral("version"),
            QStringLiteral("git hash"),
            QStringLiteral("build"),
            QStringLiteral("编译"),
            QStringLiteral("commit")
        })) {
        appendSensorStatus(QStringLiteral("version"));
    }

    if (_questionContainsAny(normalizedQuestion, {
            QStringLiteral("mavlink"),
            QStringLiteral("链路"),
            QStringLiteral("数传"),
            QStringLiteral("通信"),
            QStringLiteral("丢包"),
            QStringLiteral("link status")
        })) {
        appendSensorStatus(QStringLiteral("mavlink"));
    }

    if (_questionContainsAny(normalizedQuestion, {
            QStringLiteral("commander"),
            QStringLiteral("解锁"),
            QStringLiteral("arming"),
            QStringLiteral("preflight"),
            QStringLiteral("起飞前"),
            QStringLiteral("起飞检查"),
            QStringLiteral("不能起飞"),
            QStringLiteral("无法起飞"),
            QStringLiteral("不能解锁"),
            QStringLiteral("飞控状态"),
            QStringLiteral("能不能起飞")
        })) {
        appendSensorStatus(QStringLiteral("commander"));
    }

    bool matchedSpecificSensor = false;
    auto appendSpecificSensorStatus = [&](const QString &sensorType, const QStringList &needles) {
        if (_questionContainsAny(normalizedQuestion, needles)) {
            appendSensorStatus(sensorType);
            matchedSpecificSensor = true;
        }
    };

    appendSpecificSensorStatus(QStringLiteral("gyro"), {
        QStringLiteral("陀螺"),
        QStringLiteral("gyro"),
        QStringLiteral("gyroscope")
    });
    appendSpecificSensorStatus(QStringLiteral("accel"), {
        QStringLiteral("加速度"),
        QStringLiteral("accel"),
        QStringLiteral("accelerometer")
    });
    appendSpecificSensorStatus(QStringLiteral("mag"), {
        QStringLiteral("磁罗盘"),
        QStringLiteral("罗盘"),
        QStringLiteral("磁力"),
        QStringLiteral("compass"),
        QStringLiteral("magnetometer"),
        QStringLiteral("mag ")
    });
    appendSpecificSensorStatus(QStringLiteral("baro"), {
        QStringLiteral("气压"),
        QStringLiteral("baro"),
        QStringLiteral("barometer")
    });
    appendSpecificSensorStatus(QStringLiteral("gps"), {
        QStringLiteral("gps"),
        QStringLiteral("定位"),
        QStringLiteral("卫星"),
        QStringLiteral("rtk")
    });
    appendSpecificSensorStatus(QStringLiteral("battery"), {
        QStringLiteral("电池"),
        QStringLiteral("电压"),
        QStringLiteral("低电量"),
        QStringLiteral("battery"),
        QStringLiteral("voltage")
    });
    appendSpecificSensorStatus(QStringLiteral("estimator"), {
        QStringLiteral("ekf"),
        QStringLiteral("estimator"),
        QStringLiteral("估计器"),
        QStringLiteral("姿态估计")
    });
    appendSpecificSensorStatus(QStringLiteral("local_position"), {
        QStringLiteral("local_position"),
        QStringLiteral("local position"),
        QStringLiteral("本地位置")
    });
    appendSpecificSensorStatus(QStringLiteral("global_position"), {
        QStringLiteral("global_position"),
        QStringLiteral("global position"),
        QStringLiteral("全球位置"),
        QStringLiteral("全局位置")
    });
    appendSpecificSensorStatus(QStringLiteral("distance_sensor"), {
        QStringLiteral("distance_sensor"),
        QStringLiteral("distance sensor"),
        QStringLiteral("rangefinder"),
        QStringLiteral("测距"),
        QStringLiteral("激光")
    });
    appendSpecificSensorStatus(QStringLiteral("optical_flow"), {
        QStringLiteral("optical_flow"),
        QStringLiteral("optical flow"),
        QStringLiteral("光流")
    });

    if (!matchedSpecificSensor && _questionContainsAny(normalizedQuestion, {
            QStringLiteral("传感器"),
            QStringLiteral("sensor"),
            QStringLiteral("sensors"),
            QStringLiteral("imu")
        })) {
        appendSensorStatus(QStringLiteral("all"));
    }

    return toolCommands;
}

MAVLinkConsoleAIController::PendingConsoleToolCommand MAVLinkConsoleAIController::_makeSensorStatusToolCommand(const QString &toolCallId, const QString &sensorType) const
{
    const QString normalizedSensorType = _normalizedSensorType(sensorType);
    PendingConsoleToolCommand toolCommand;
    toolCommand.toolCallId = toolCallId;
    toolCommand.toolName = QStringLiteral("query_sensor_status");
    toolCommand.arguments = QJsonObject{
        { QStringLiteral("sensor_type"), normalizedSensorType }
    };
    toolCommand.executionKind = ToolExecutionConsole;
    toolCommand.command = _consoleCommandForSensorStatus(normalizedSensorType);
    return toolCommand;
}

MAVLinkConsoleAIController::PendingConsoleToolCommand MAVLinkConsoleAIController::_makeParamToolCommand(const QString &toolCallId, const QString &paramName) const
{
    const QString normalizedParamName = paramName.trimmed().toUpper();
    PendingConsoleToolCommand toolCommand;
    toolCommand.toolCallId = toolCallId;
    toolCommand.toolName = QStringLiteral("query_px4_param");
    toolCommand.arguments = QJsonObject{
        { QStringLiteral("param_name"), normalizedParamName }
    };
    toolCommand.executionKind = ToolExecutionConsole;
    if (_isSafePx4ParameterName(normalizedParamName)) {
        toolCommand.command = QStringLiteral("param show %1").arg(normalizedParamName);
    }
    return toolCommand;
}

QJsonObject MAVLinkConsoleAIController::_toolCallMessageForCommands(const QList<PendingConsoleToolCommand> &toolCommands) const
{
    QJsonArray toolCalls;
    for (const PendingConsoleToolCommand &toolCommand : toolCommands) {
        toolCalls.append(_toolCallObjectForCommand(toolCommand));
    }

    return QJsonObject{
        { QStringLiteral("role"), QStringLiteral("assistant") },
        { QStringLiteral("content"), tr("Running matching PX4 read-only diagnostic queries.") },
        { QStringLiteral("tool_calls"), toolCalls }
    };
}

QJsonObject MAVLinkConsoleAIController::_toolCallObjectForCommand(const PendingConsoleToolCommand &toolCommand) const
{
    return QJsonObject{
        { QStringLiteral("id"), toolCommand.toolCallId },
        { QStringLiteral("type"), QStringLiteral("function") },
        { QStringLiteral("function"), QJsonObject{
            { QStringLiteral("name"), toolCommand.toolName },
            { QStringLiteral("arguments"), QString::fromUtf8(QJsonDocument(toolCommand.arguments).toJson(QJsonDocument::Compact)) }
        } }
    };
}

QString MAVLinkConsoleAIController::_firstPx4ParameterNameInQuestion(const QString &question) const
{
    static const QRegularExpression paramRegex(QStringLiteral("\\b[A-Za-z][A-Za-z0-9_]{2,31}\\b"));
    QRegularExpressionMatchIterator iterator = paramRegex.globalMatch(question);
    while (iterator.hasNext()) {
        const QString candidate = iterator.next().captured(0).trimmed().toUpper();
        if (candidate.contains(QChar('_')) && _isSafePx4ParameterName(candidate)) {
            return candidate;
        }
    }

    return QString();
}

bool MAVLinkConsoleAIController::_questionContainsAny(const QString &normalizedQuestion, const QStringList &needles) const
{
    for (const QString &needle : needles) {
        if (normalizedQuestion.contains(needle, Qt::CaseInsensitive)) {
            return true;
        }
    }

    return false;
}

void MAVLinkConsoleAIController::_startToolExecution(const QJsonArray &toolCalls)
{
    _automaticToolExecution = false;
    _pendingToolResultMessages = QJsonArray();
    _pendingConsoleToolCommands.clear();
    _clearPendingApproval();

    for (const QJsonValue &toolCallValue : toolCalls) {
        const QJsonObject toolCall = toolCallValue.toObject();
        PendingConsoleToolCommand toolCommand;
        QJsonObject immediateResult;
        if (_buildAIToolCommand(toolCall, &toolCommand, &immediateResult)) {
            if (toolCommand.executionKind == ToolExecutionImmediate) {
                immediateResult = _executeImmediateToolCommand(toolCommand);
            } else if (toolCommand.executionKind == ToolExecutionConsole) {
                if (_remainingConsoleCommands <= 0) {
                    immediateResult = _toolResultObject(
                        toolCommand.toolCallId,
                        toolCommand.toolName,
                        toolCommand.arguments,
                        QStringLiteral("rejected"),
                        tr("The read-only diagnostic command limit for this question was reached."),
                        toolCommand.command);
                } else {
                    _pendingConsoleToolCommands.append(toolCommand);
                    _remainingConsoleCommands--;
                    continue;
                }
            } else if (_toolRequiresUserApproval(toolCommand)) {
                if (_remainingLowPrivilegeMavlinkCommands <= 0) {
                    immediateResult = _toolResultObject(
                        toolCommand.toolCallId,
                        toolCommand.toolName,
                        toolCommand.arguments,
                        QStringLiteral("rejected"),
                        tr("The low-privilege MAVLink command limit for this question was reached."),
                        toolCommand.command);
                } else {
                    _remainingLowPrivilegeMavlinkCommands--;
                    _setPendingApproval(toolCommand);
                    return;
                }
            } else {
                immediateResult = _toolResultObject(
                    toolCommand.toolCallId,
                    toolCommand.toolName,
                    toolCommand.arguments,
                    QStringLiteral("rejected"),
                    tr("The requested AI tool cannot be executed."),
                    toolCommand.command);
            }
        }

        const QString toolCallId = toolCall.value(QStringLiteral("id")).toString();
        _pendingToolResultMessages.append(QJsonObject{
            { QStringLiteral("role"), QStringLiteral("tool") },
            { QStringLiteral("tool_call_id"), toolCallId },
            { QStringLiteral("content"), QString::fromUtf8(QJsonDocument(immediateResult).toJson(QJsonDocument::Compact)) }
        });
    }

    _executeNextConsoleToolCommand();
}

void MAVLinkConsoleAIController::_appendToolResult(const PendingConsoleToolCommand &toolCommand, const QJsonObject &result)
{
    if (_automaticToolExecution) {
        _pendingToolResultMessages.append(result);
        return;
    }

    _pendingToolResultMessages.append(QJsonObject{
        { QStringLiteral("role"), QStringLiteral("tool") },
        { QStringLiteral("tool_call_id"), toolCommand.toolCallId },
        { QStringLiteral("content"), QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)) }
    });
}

bool MAVLinkConsoleAIController::_buildAIToolCommand(const QJsonObject &toolCall, PendingConsoleToolCommand *toolCommand, QJsonObject *immediateResult) const
{
    if (toolCommand) {
        *toolCommand = PendingConsoleToolCommand();
    }
    if (immediateResult) {
        *immediateResult = QJsonObject();
    }

    const QString toolCallId = toolCall.value(QStringLiteral("id")).toString();
    const QJsonObject functionObject = toolCall.value(QStringLiteral("function")).toObject();
    const QString toolName = functionObject.value(QStringLiteral("name")).toString();

    QJsonParseError parseError{};
    const QJsonDocument argumentsDocument = QJsonDocument::fromJson(functionObject.value(QStringLiteral("arguments")).toString().toUtf8(), &parseError);
    const QJsonObject arguments = (parseError.error == QJsonParseError::NoError && argumentsDocument.isObject())
        ? argumentsDocument.object()
        : QJsonObject();

    auto reject = [&](const QString &message, const QString &command = QString()) {
        if (immediateResult) {
            *immediateResult = _toolResultObject(toolCallId, toolName, arguments, QStringLiteral("rejected"), message, command);
        }
        return false;
    };

    if (toolCallId.isEmpty() || toolName.isEmpty()) {
        return reject(tr("The AI tool request was malformed."));
    }

    Vehicle *vehicle = MultiVehicleManager::instance()->activeVehicle();

    PendingConsoleToolCommand builtCommand;
    builtCommand.toolCallId = toolCallId;
    builtCommand.toolName = toolName;
    builtCommand.arguments = arguments;

    QString command;

    if (toolName == QStringLiteral("get_vehicle_status") ||
        toolName == QStringLiteral("get_fact_group") ||
        toolName == QStringLiteral("get_parameter") ||
        toolName == QStringLiteral("search_parameters") ||
        toolName == QStringLiteral("get_health_report") ||
        toolName == QStringLiteral("get_link_status")) {
        builtCommand.executionKind = ToolExecutionImmediate;
    } else if (toolName == QStringLiteral("query_sensor_status")) {
        if (!vehicle) {
            return reject(tr("No active vehicle is connected."));
        }
        if (vehicle->firmwareType() != MAV_AUTOPILOT_PX4) {
            return reject(tr("The read-only PX4 shell diagnostic tools only support PX4 vehicles."));
        }
        if (vehicle->armed() || vehicle->flying()) {
            return reject(tr("The vehicle is armed or flying, so automatic PX4 console diagnostics were skipped. Use the provided QGroundControl telemetry snapshot instead."));
        }
        if (vehicle->vehicleLinkManager()->communicationLost()) {
            return reject(tr("Vehicle communication is currently lost."));
        }

        command = _consoleCommandForSensorStatus(arguments.value(QStringLiteral("sensor_type")).toString());
        if (command.isEmpty()) {
            return reject(tr("The requested PX4 diagnostic target is not in the read-only whitelist."));
        }
        builtCommand.executionKind = ToolExecutionConsole;
        builtCommand.command = command;
    } else if (toolName == QStringLiteral("query_px4_param")) {
        if (!vehicle) {
            return reject(tr("No active vehicle is connected."));
        }
        if (vehicle->firmwareType() != MAV_AUTOPILOT_PX4) {
            return reject(tr("The read-only PX4 shell diagnostic tools only support PX4 vehicles."));
        }
        if (vehicle->armed() || vehicle->flying()) {
            return reject(tr("The vehicle is armed or flying, so automatic PX4 console diagnostics were skipped. Use the provided QGroundControl telemetry snapshot instead."));
        }
        if (vehicle->vehicleLinkManager()->communicationLost()) {
            return reject(tr("Vehicle communication is currently lost."));
        }

        const QString paramName = arguments.value(QStringLiteral("param_name")).toString().trimmed().toUpper();
        if (!_isSafePx4ParameterName(paramName)) {
            return reject(tr("The requested PX4 parameter name is invalid."), QStringLiteral("param show %1").arg(paramName));
        }
        command = QStringLiteral("param show %1").arg(paramName);
        builtCommand.arguments.insert(QStringLiteral("param_name"), paramName);
        builtCommand.executionKind = ToolExecutionConsole;
        builtCommand.command = command;
    } else if (toolName == QStringLiteral("request_mavlink_message")) {
        QString errorText;
        if (!_validateVehicleForAITool(vehicle, toolName, &errorText)) {
            return reject(errorText);
        }
        const int messageId = arguments.value(QStringLiteral("message_id")).toInt(-1);
        if (!_isSafeMavlinkMessageId(messageId)) {
            return reject(tr("The requested MAVLink message id is not in the low-privilege data whitelist."));
        }
        const int componentId = arguments.contains(QStringLiteral("component_id"))
            ? arguments.value(QStringLiteral("component_id")).toInt(vehicle->defaultComponentId())
            : vehicle->defaultComponentId();
        if (componentId <= 0 || componentId == MAV_COMP_ID_ALL) {
            return reject(tr("The requested MAVLink component id is invalid for this tool."));
        }

        builtCommand.executionKind = ToolExecutionRequestMessage;
        builtCommand.componentId = componentId;
        builtCommand.messageId = messageId;
        builtCommand.command = QStringLiteral("MAV_CMD_REQUEST_MESSAGE component=%1 message_id=%2").arg(componentId).arg(messageId);
        builtCommand.arguments.insert(QStringLiteral("component_id"), componentId);
        builtCommand.arguments.insert(QStringLiteral("message_id"), messageId);
    } else if (toolName == QStringLiteral("set_message_interval")) {
        QString errorText;
        if (!_validateVehicleForAITool(vehicle, toolName, &errorText)) {
            return reject(errorText);
        }
        const int messageId = arguments.value(QStringLiteral("message_id")).toInt(-1);
        if (!_isSafeMavlinkMessageId(messageId)) {
            return reject(tr("The requested MAVLink message id is not in the low-privilege telemetry whitelist."));
        }
        const double rateHz = arguments.value(QStringLiteral("rate_hz")).toDouble(-1.0);
        if (!(rateHz > 0.0) || rateHz > kMaxMavlinkMessageIntervalHz) {
            return reject(tr("The requested MAVLink stream rate must be greater than 0 and no more than %1 Hz.").arg(kMaxMavlinkMessageIntervalHz));
        }
        int ttlSeconds = arguments.value(QStringLiteral("ttl_seconds")).toInt(10);
        ttlSeconds = qBound(1, ttlSeconds, kMaxMavlinkMessageIntervalTtlSeconds);
        const int componentId = arguments.contains(QStringLiteral("component_id"))
            ? arguments.value(QStringLiteral("component_id")).toInt(vehicle->defaultComponentId())
            : vehicle->defaultComponentId();
        if (componentId <= 0 || componentId == MAV_COMP_ID_ALL) {
            return reject(tr("The requested MAVLink component id is invalid for this tool."));
        }

        builtCommand.executionKind = ToolExecutionSetMessageInterval;
        builtCommand.componentId = componentId;
        builtCommand.messageId = messageId;
        builtCommand.intervalUsec = qMax(1, static_cast<int>(1000000.0 / rateHz));
        builtCommand.ttlSeconds = ttlSeconds;
        builtCommand.command = QStringLiteral("MAV_CMD_SET_MESSAGE_INTERVAL component=%1 message_id=%2 interval_us=%3 ttl_s=%4")
                                   .arg(componentId)
                                   .arg(messageId)
                                   .arg(builtCommand.intervalUsec)
                                   .arg(ttlSeconds);
        builtCommand.arguments.insert(QStringLiteral("component_id"), componentId);
        builtCommand.arguments.insert(QStringLiteral("message_id"), messageId);
        builtCommand.arguments.insert(QStringLiteral("interval_usec"), builtCommand.intervalUsec);
        builtCommand.arguments.insert(QStringLiteral("ttl_seconds"), ttlSeconds);
    } else {
        return reject(tr("The requested AI tool is not supported."));
    }

    if (builtCommand.executionKind == ToolExecutionConsole && !_isWhitelistedConsoleCommand(builtCommand.command)) {
        return reject(tr("The mapped PX4 console command is not in the read-only whitelist."), builtCommand.command);
    }

    if (toolCommand) {
        *toolCommand = builtCommand;
    }
    return true;
}

QJsonObject MAVLinkConsoleAIController::_executeImmediateToolCommand(const PendingConsoleToolCommand &toolCommand) const
{
    Vehicle *vehicle = MultiVehicleManager::instance()->activeVehicle();

    auto okResult = [&](const QString &message, const QJsonValue &data) {
        QJsonObject result = _toolResultObject(toolCommand.toolCallId, toolCommand.toolName, toolCommand.arguments, QStringLiteral("ok"), message);
        result.insert(QStringLiteral("data"), data);
        return result;
    };
    auto errorResult = [&](const QString &message) {
        return _toolResultObject(toolCommand.toolCallId, toolCommand.toolName, toolCommand.arguments, QStringLiteral("error"), message);
    };

    if (toolCommand.toolName == QStringLiteral("get_vehicle_status")) {
        return okResult(tr("Current QGroundControl active vehicle status snapshot."), _buildVehicleSnapshot(vehicle));
    }

    if (!vehicle) {
        return errorResult(tr("No active vehicle is connected."));
    }
    if (vehicle->firmwareType() != MAV_AUTOPILOT_PX4) {
        return errorResult(tr("The AI vehicle information tools currently support PX4 vehicles only."));
    }

    if (toolCommand.toolName == QStringLiteral("get_fact_group")) {
        const QString groupName = toolCommand.arguments.value(QStringLiteral("group_name")).toString().trimmed();
        if (groupName.isEmpty()) {
            return errorResult(tr("The requested FactGroup name is empty."));
        }

        const FactGroup *factGroup = nullptr;
        if (groupName == QStringLiteral("vehicle")) {
            factGroup = vehicle;
        } else {
            factGroup = vehicle->getFactGroup(groupName);
        }

        if (!factGroup) {
            QJsonObject data;
            data.insert(QStringLiteral("requestedGroup"), groupName);
            data.insert(QStringLiteral("availableGroups"), QJsonArray::fromStringList(QStringList{ QStringLiteral("vehicle") } + vehicle->factGroupNames()));
            return okResult(tr("The requested FactGroup is not available on the active vehicle."), data);
        }

        QJsonObject data = _factGroupToJson(factGroup);
        data.insert(QStringLiteral("groupName"), groupName);
        return okResult(tr("QGroundControl FactGroup returned."), data);
    }

    if (toolCommand.toolName == QStringLiteral("get_parameter") || toolCommand.toolName == QStringLiteral("query_px4_param")) {
        const QString paramName = toolCommand.arguments.value(QStringLiteral("param_name")).toString().trimmed().toUpper();
        const int componentId = toolCommand.arguments.contains(QStringLiteral("component_id"))
            ? toolCommand.arguments.value(QStringLiteral("component_id")).toInt(ParameterManager::defaultComponentId)
            : ParameterManager::defaultComponentId;
        if (!_isSafePx4ParameterName(paramName)) {
            return errorResult(tr("The requested PX4 parameter name is invalid."));
        }
        ParameterManager *parameterManager = vehicle->parameterManager();
        if (!parameterManager || !parameterManager->parametersReady()) {
            return errorResult(tr("Vehicle parameters are not ready yet."));
        }
        if (!parameterManager->parameterExists(componentId, paramName)) {
            QJsonObject data;
            data.insert(QStringLiteral("paramName"), paramName);
            data.insert(QStringLiteral("componentId"), componentId);
            return okResult(tr("The requested parameter is not available in QGroundControl's loaded parameters."), data);
        }

        return okResult(tr("PX4 parameter returned from QGroundControl ParameterManager."), _parameterToJson(parameterManager->getParameter(componentId, paramName)));
    }

    if (toolCommand.toolName == QStringLiteral("search_parameters")) {
        const QString query = toolCommand.arguments.value(QStringLiteral("query")).toString().trimmed().toUpper();
        const int componentId = toolCommand.arguments.contains(QStringLiteral("component_id"))
            ? toolCommand.arguments.value(QStringLiteral("component_id")).toInt(ParameterManager::defaultComponentId)
            : ParameterManager::defaultComponentId;
        const int limit = qBound(1, toolCommand.arguments.value(QStringLiteral("limit")).toInt(10), 25);
        if (query.isEmpty()) {
            return errorResult(tr("The parameter search query is empty."));
        }

        ParameterManager *parameterManager = vehicle->parameterManager();
        if (!parameterManager || !parameterManager->parametersReady()) {
            return errorResult(tr("Vehicle parameters are not ready yet."));
        }

        QStringList names = parameterManager->parameterNames(componentId);
        names.sort(Qt::CaseInsensitive);

        QJsonArray matches;
        for (const QString &name : names) {
            if (!name.contains(query, Qt::CaseInsensitive)) {
                continue;
            }
            if (!parameterManager->parameterExists(componentId, name)) {
                continue;
            }
            matches.append(_parameterToJson(parameterManager->getParameter(componentId, name)));
            if (matches.size() >= limit) {
                break;
            }
        }

        QJsonObject data;
        data.insert(QStringLiteral("query"), query);
        data.insert(QStringLiteral("componentId"), componentId);
        data.insert(QStringLiteral("limit"), limit);
        data.insert(QStringLiteral("matches"), matches);
        data.insert(QStringLiteral("truncated"), matches.size() >= limit);
        return okResult(tr("PX4 parameter search results returned from QGroundControl ParameterManager."), data);
    }

    if (toolCommand.toolName == QStringLiteral("get_health_report")) {
        return okResult(tr("PX4 health and arming check report returned."), _buildHealthAndArmingCheckReportJson(vehicle));
    }

    if (toolCommand.toolName == QStringLiteral("get_link_status")) {
        return okResult(tr("QGroundControl MAVLink link status returned."), _buildLinkStatusJson(vehicle));
    }

    return errorResult(tr("The requested AI tool is not implemented."));
}

QString MAVLinkConsoleAIController::_consoleCommandForSensorStatus(const QString &sensorType) const
{
    const QString normalizedSensorType = _normalizedSensorType(sensorType);
    if (normalizedSensorType == QStringLiteral("all")) {
        return QStringLiteral("sensors status");
    }
    if (normalizedSensorType == QStringLiteral("accel")) {
        return QStringLiteral("listener sensor_accel 1");
    }
    if (normalizedSensorType == QStringLiteral("gyro")) {
        return QStringLiteral("listener sensor_gyro 1");
    }
    if (normalizedSensorType == QStringLiteral("mag")) {
        return QStringLiteral("listener sensor_mag 1");
    }
    if (normalizedSensorType == QStringLiteral("baro")) {
        return QStringLiteral("listener sensor_baro 1");
    }
    if (normalizedSensorType == QStringLiteral("gps")) {
        return QStringLiteral("listener sensor_gps 1");
    }
    if (normalizedSensorType == QStringLiteral("battery")) {
        return QStringLiteral("listener battery_status 1");
    }
    if (normalizedSensorType == QStringLiteral("estimator")) {
        return QStringLiteral("listener estimator_status 1");
    }
    if (normalizedSensorType == QStringLiteral("local_position")) {
        return QStringLiteral("listener vehicle_local_position 1");
    }
    if (normalizedSensorType == QStringLiteral("global_position")) {
        return QStringLiteral("listener vehicle_global_position 1");
    }
    if (normalizedSensorType == QStringLiteral("distance_sensor")) {
        return QStringLiteral("listener distance_sensor 1");
    }
    if (normalizedSensorType == QStringLiteral("optical_flow")) {
        return QStringLiteral("listener sensor_optical_flow 1");
    }
    if (normalizedSensorType == QStringLiteral("commander")) {
        return QStringLiteral("commander status");
    }
    if (normalizedSensorType == QStringLiteral("mavlink")) {
        return QStringLiteral("mavlink status");
    }
    if (normalizedSensorType == QStringLiteral("version")) {
        return QStringLiteral("ver all");
    }

    return QString();
}

QString MAVLinkConsoleAIController::_normalizedSensorType(const QString &sensorType) const
{
    const QString normalized = sensorType.trimmed().toLower().replace(QChar::Space, QChar('_')).replace(QChar('-'), QChar('_'));
    if (normalized == QStringLiteral("imu")) {
        return QStringLiteral("all");
    }
    if (normalized == QStringLiteral("accelerometer")) {
        return QStringLiteral("accel");
    }
    if (normalized == QStringLiteral("gyroscope")) {
        return QStringLiteral("gyro");
    }
    if (normalized == QStringLiteral("compass") || normalized == QStringLiteral("magnetometer")) {
        return QStringLiteral("mag");
    }
    if (normalized == QStringLiteral("barometer")) {
        return QStringLiteral("baro");
    }
    if (normalized == QStringLiteral("ekf") || normalized == QStringLiteral("ekf2")) {
        return QStringLiteral("estimator");
    }
    if (normalized == QStringLiteral("localposition")) {
        return QStringLiteral("local_position");
    }
    if (normalized == QStringLiteral("globalposition")) {
        return QStringLiteral("global_position");
    }
    if (normalized == QStringLiteral("rangefinder") || normalized == QStringLiteral("distance")) {
        return QStringLiteral("distance_sensor");
    }
    if (normalized == QStringLiteral("flow")) {
        return QStringLiteral("optical_flow");
    }
    return normalized;
}

bool MAVLinkConsoleAIController::_isSafePx4ParameterName(const QString &paramName) const
{
    static const QRegularExpression paramNameRegex(QStringLiteral("^[A-Z][A-Z0-9_]{0,31}$"));
    return paramNameRegex.match(paramName).hasMatch();
}

bool MAVLinkConsoleAIController::_isWhitelistedConsoleCommand(const QString &command) const
{
    static const QStringList exactCommands{
        QStringLiteral("sensors status"),
        QStringLiteral("listener sensor_accel 1"),
        QStringLiteral("listener sensor_gyro 1"),
        QStringLiteral("listener sensor_mag 1"),
        QStringLiteral("listener sensor_baro 1"),
        QStringLiteral("listener sensor_gps 1"),
        QStringLiteral("listener battery_status 1"),
        QStringLiteral("listener estimator_status 1"),
        QStringLiteral("listener vehicle_local_position 1"),
        QStringLiteral("listener vehicle_global_position 1"),
        QStringLiteral("listener distance_sensor 1"),
        QStringLiteral("listener sensor_optical_flow 1"),
        QStringLiteral("commander status"),
        QStringLiteral("mavlink status"),
        QStringLiteral("ver all")
    };

    if (exactCommands.contains(command)) {
        return true;
    }

    static const QRegularExpression paramShowRegex(QStringLiteral("^param show [A-Z][A-Z0-9_]{0,31}$"));
    return paramShowRegex.match(command).hasMatch();
}

bool MAVLinkConsoleAIController::_isSafeMavlinkMessageId(int messageId) const
{
    switch (messageId) {
    case MAVLINK_MSG_ID_HEARTBEAT:
    case MAVLINK_MSG_ID_SYS_STATUS:
    case MAVLINK_MSG_ID_SYSTEM_TIME:
    case MAVLINK_MSG_ID_GPS_RAW_INT:
    case MAVLINK_MSG_ID_ATTITUDE:
    case MAVLINK_MSG_ID_LOCAL_POSITION_NED:
    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT:
    case MAVLINK_MSG_ID_HOME_POSITION:
    case MAVLINK_MSG_ID_VFR_HUD:
    case MAVLINK_MSG_ID_EXTENDED_SYS_STATE:
    case MAVLINK_MSG_ID_BATTERY_STATUS:
    case MAVLINK_MSG_ID_AUTOPILOT_VERSION:
    case MAVLINK_MSG_ID_ESTIMATOR_STATUS:
        return true;
    default:
        return false;
    }
}

bool MAVLinkConsoleAIController::_validateVehicleForAITool(Vehicle *vehicle, const QString &toolName, QString *errorText) const
{
    auto reject = [&](const QString &message) {
        if (errorText) {
            *errorText = message;
        }
        return false;
    };

    if (!vehicle) {
        return reject(tr("No active vehicle is connected."));
    }
    if (vehicle->firmwareType() != MAV_AUTOPILOT_PX4) {
        return reject(tr("Low-privilege AI MAVLink tools currently support PX4 vehicles only."));
    }
    if (vehicle->armed() || vehicle->flying()) {
        return reject(tr("The vehicle is armed or flying, so low-privilege AI MAVLink tools are disabled."));
    }
    if (vehicle->vehicleLinkManager()->communicationLost()) {
        return reject(tr("Vehicle communication is currently lost."));
    }
    if (toolName != QStringLiteral("request_mavlink_message") && toolName != QStringLiteral("set_message_interval")) {
        return reject(tr("The requested low-privilege MAVLink tool is not supported."));
    }

    return true;
}

bool MAVLinkConsoleAIController::_toolRequiresUserApproval(const PendingConsoleToolCommand &toolCommand) const
{
    return toolCommand.executionKind == ToolExecutionRequestMessage ||
           toolCommand.executionKind == ToolExecutionSetMessageInterval;
}

void MAVLinkConsoleAIController::_setPendingApproval(const PendingConsoleToolCommand &toolCommand)
{
    _pendingApprovalToolCommand = toolCommand;
    _pendingActionAvailable = true;
    _pendingActionCommand = toolCommand.command;
    _pendingActionRisk = tr("Low privilege: data request only. It cannot arm, move, change mode, write parameters, modify missions, or drive actuators.");

    if (toolCommand.executionKind == ToolExecutionRequestMessage) {
        _pendingActionTitle = tr("Request MAVLink Message");
        _pendingActionDescription = tr("The AI wants to request one whitelisted MAVLink data message from component %1: message id %2.")
                                        .arg(toolCommand.componentId)
                                        .arg(toolCommand.messageId);
    } else if (toolCommand.executionKind == ToolExecutionSetMessageInterval) {
        _pendingActionTitle = tr("Temporarily Stream MAVLink Message");
        _pendingActionDescription = tr("The AI wants to request message id %1 from component %2 every %3 microseconds for %4 seconds, then restore the default interval.")
                                        .arg(toolCommand.messageId)
                                        .arg(toolCommand.componentId)
                                        .arg(toolCommand.intervalUsec)
                                        .arg(toolCommand.ttlSeconds);
    } else {
        _pendingActionTitle = tr("Confirm AI Action");
        _pendingActionDescription = tr("The AI requested an action that requires user confirmation.");
    }

    emit pendingActionChanged();
}

void MAVLinkConsoleAIController::_clearPendingApproval()
{
    if (!_pendingActionAvailable &&
        _pendingActionTitle.isEmpty() &&
        _pendingActionDescription.isEmpty() &&
        _pendingActionCommand.isEmpty() &&
        _pendingActionRisk.isEmpty()) {
        return;
    }

    _pendingActionAvailable = false;
    _pendingApprovalToolCommand = PendingConsoleToolCommand();
    _pendingActionTitle.clear();
    _pendingActionDescription.clear();
    _pendingActionCommand.clear();
    _pendingActionRisk.clear();
    emit pendingActionChanged();
}

QJsonObject MAVLinkConsoleAIController::_toolResultObject(const QString &toolCallId, const QString &toolName, const QJsonObject &arguments, const QString &status, const QString &message, const QString &command, const QString &output, bool outputTruncated) const
{
    QJsonObject result{
        { QStringLiteral("source"), QStringLiteral("QGroundControl AI vehicle tool registry") },
        { QStringLiteral("timestampUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) },
        { QStringLiteral("toolCallId"), toolCallId },
        { QStringLiteral("tool"), toolName },
        { QStringLiteral("arguments"), arguments },
        { QStringLiteral("status"), status },
        { QStringLiteral("message"), message }
    };

    if (Vehicle *vehicle = MultiVehicleManager::instance()->activeVehicle()) {
        result.insert(QStringLiteral("vehicleId"), vehicle->id());
    }
    if (!command.isEmpty()) {
        result.insert(QStringLiteral("command"), command);
    }
    if (!output.isEmpty()) {
        result.insert(QStringLiteral("output"), output);
        result.insert(QStringLiteral("outputTruncated"), outputTruncated);
    }
    return result;
}

void MAVLinkConsoleAIController::_executeNextConsoleToolCommand()
{
    if (_pendingConsoleToolCommands.isEmpty()) {
        _finishToolExecution();
        return;
    }

    Vehicle *vehicle = MultiVehicleManager::instance()->activeVehicle();
    if (!vehicle) {
        while (!_pendingConsoleToolCommands.isEmpty()) {
            const PendingConsoleToolCommand toolCommand = _pendingConsoleToolCommands.takeFirst();
            const QJsonObject result = _toolResultObject(toolCommand.toolCallId, toolCommand.toolName, toolCommand.arguments, QStringLiteral("error"), tr("No active vehicle is connected."), toolCommand.command);
            _appendToolResult(toolCommand, result);
        }
        _finishToolExecution();
        return;
    }

    _activeConsoleToolCommand = _pendingConsoleToolCommands.takeFirst();
    _activeConsoleOutput.clear();
    _activeConsoleOutputTruncated = false;

    QString rejectMessage;
    if (vehicle->firmwareType() != MAV_AUTOPILOT_PX4) {
        rejectMessage = tr("The read-only diagnostic tools only support PX4 vehicles.");
    } else if (vehicle->armed() || vehicle->flying()) {
        rejectMessage = tr("The vehicle is armed or flying, so automatic PX4 console diagnostics were skipped. Use the provided QGroundControl telemetry snapshot instead.");
    } else if (vehicle->vehicleLinkManager()->communicationLost()) {
        rejectMessage = tr("Vehicle communication is currently lost.");
    }

    if (!rejectMessage.isEmpty()) {
        const QJsonObject result = _toolResultObject(
            _activeConsoleToolCommand.toolCallId,
            _activeConsoleToolCommand.toolName,
            _activeConsoleToolCommand.arguments,
            QStringLiteral("rejected"),
            rejectMessage,
            _activeConsoleToolCommand.command);
        _appendToolResult(_activeConsoleToolCommand, result);
        _activeConsoleToolCommand = PendingConsoleToolCommand();
        _executeNextConsoleToolCommand();
        return;
    }

    _consoleDataConnection = connect(vehicle, &Vehicle::mavlinkSerialControl, this, &MAVLinkConsoleAIController::_receiveConsoleData);
    _sendSerialData(_activeConsoleToolCommand.command.toUtf8() + QByteArrayLiteral("\n"));
    _consoleCommandTimer.start();
}

void MAVLinkConsoleAIController::_executePendingMavlinkToolCommand(const PendingConsoleToolCommand &toolCommand)
{
    Vehicle *vehicle = MultiVehicleManager::instance()->activeVehicle();
    QString errorText;
    if (!_validateVehicleForAITool(vehicle, toolCommand.toolName, &errorText)) {
        _appendToolResult(toolCommand, _toolResultObject(
            toolCommand.toolCallId,
            toolCommand.toolName,
            toolCommand.arguments,
            QStringLiteral("rejected"),
            errorText,
            toolCommand.command));
        _finishToolExecution();
        return;
    }

    _activeMavlinkToolCommand = toolCommand;

    auto *callbackData = new AIToolCallbackData();
    callbackData->controller = this;
    callbackData->vehicle = vehicle;
    callbackData->toolCommand = toolCommand;

    if (toolCommand.executionKind == ToolExecutionRequestMessage) {
        vehicle->requestMessage(
            &MAVLinkConsoleAIController::_requestMessageResultHandler,
            callbackData,
            toolCommand.componentId,
            toolCommand.messageId);
        return;
    }

    if (toolCommand.executionKind == ToolExecutionSetMessageInterval) {
        Vehicle::MavCmdAckHandlerInfo_t handlerInfo{};
        handlerInfo.resultHandler = &MAVLinkConsoleAIController::_mavCommandResultHandler;
        handlerInfo.resultHandlerData = callbackData;
        vehicle->sendMavCommandWithHandler(
            &handlerInfo,
            toolCommand.componentId,
            MAV_CMD_SET_MESSAGE_INTERVAL,
            static_cast<float>(toolCommand.messageId),
            static_cast<float>(toolCommand.intervalUsec));
        return;
    }

    delete callbackData;
    _appendToolResult(toolCommand, _toolResultObject(
        toolCommand.toolCallId,
        toolCommand.toolName,
        toolCommand.arguments,
        QStringLiteral("rejected"),
        tr("The requested AI MAVLink action is not executable."),
        toolCommand.command));
    _finishToolExecution();
}

void MAVLinkConsoleAIController::_consoleCommandTimedOut()
{
    _finishActiveConsoleToolCommand(QStringLiteral("ok"), tr("PX4 read-only diagnostic command completed or timed out after the capture window."));
}

void MAVLinkConsoleAIController::_receiveConsoleData(uint8_t device, uint8_t flags, uint16_t timeout, uint32_t baudrate, const QByteArray &data)
{
    Q_UNUSED(flags);
    Q_UNUSED(timeout);
    Q_UNUSED(baudrate);

    if (device != SERIAL_CONTROL_DEV_SHELL) {
        return;
    }

    _activeConsoleOutput.append(data);
    if (_activeConsoleOutput.size() > (kConsoleCommandOutputMaxChars * 2)) {
        _activeConsoleOutput.remove(0, _activeConsoleOutput.size() - kConsoleCommandOutputMaxChars);
        _activeConsoleOutputTruncated = true;
    }
}

void MAVLinkConsoleAIController::_finishActiveConsoleToolCommand(const QString &status, const QString &message)
{
    _consoleCommandTimer.stop();
    if (_consoleDataConnection) {
        (void) disconnect(_consoleDataConnection);
        _consoleDataConnection = QMetaObject::Connection();
    }

    bool outputTruncated = _activeConsoleOutputTruncated;
    const QString output = _cleanConsoleOutput(_activeConsoleOutput, &outputTruncated);
    const QJsonObject result = _toolResultObject(
        _activeConsoleToolCommand.toolCallId,
        _activeConsoleToolCommand.toolName,
        _activeConsoleToolCommand.arguments,
        status,
        message,
        _activeConsoleToolCommand.command,
        output,
        outputTruncated);

    _appendToolResult(_activeConsoleToolCommand, result);

    _activeConsoleToolCommand = PendingConsoleToolCommand();
    _activeConsoleOutput.clear();
    _activeConsoleOutputTruncated = false;
    _executeNextConsoleToolCommand();
}

void MAVLinkConsoleAIController::_finishActiveMavlinkToolCommand(const PendingConsoleToolCommand &toolCommand, const QJsonObject &result)
{
    _appendToolResult(toolCommand, result);
    _activeMavlinkToolCommand = PendingConsoleToolCommand();
    _finishToolExecution();
}

void MAVLinkConsoleAIController::_requestMessageResultHandler(void *resultHandlerData, MAV_RESULT commandResult, Vehicle::RequestMessageResultHandlerFailureCode_t failureCode, const mavlink_message_t &message)
{
    AIToolCallbackData *data = static_cast<AIToolCallbackData*>(resultHandlerData);
    if (!data) {
        return;
    }

    QPointer<MAVLinkConsoleAIController> controller = data->controller;
    const PendingConsoleToolCommand toolCommand = data->toolCommand;
    delete data;

    if (!controller) {
        return;
    }
    if (controller->_activeMavlinkToolCommand.toolCallId != toolCommand.toolCallId ||
        controller->_activeMavlinkToolCommand.toolName != toolCommand.toolName) {
        return;
    }

    QJsonObject result = controller->_toolResultObject(
        toolCommand.toolCallId,
        toolCommand.toolName,
        toolCommand.arguments,
        failureCode == Vehicle::RequestMessageNoFailure ? QStringLiteral("ok") : QStringLiteral("error"),
        failureCode == Vehicle::RequestMessageNoFailure
            ? controller->tr("MAV_CMD_REQUEST_MESSAGE completed and the requested message was received.")
            : controller->tr("MAV_CMD_REQUEST_MESSAGE did not return the requested message."),
        toolCommand.command);

    QJsonObject dataObject;
    dataObject.insert(QStringLiteral("commandResult"), mavResultText(commandResult));
    dataObject.insert(QStringLiteral("failureCode"), requestMessageFailureText(failureCode));
    if (failureCode == Vehicle::RequestMessageNoFailure) {
        dataObject.insert(QStringLiteral("message"), controller->_mavlinkMessageToJson(message));
    }
    result.insert(QStringLiteral("data"), dataObject);

    controller->_finishActiveMavlinkToolCommand(toolCommand, result);
}

void MAVLinkConsoleAIController::_mavCommandResultHandler(void *resultHandlerData, int compId, const mavlink_command_ack_t &ack, Vehicle::MavCmdResultFailureCode_t failureCode)
{
    AIToolCallbackData *data = static_cast<AIToolCallbackData*>(resultHandlerData);
    if (!data) {
        return;
    }

    QPointer<MAVLinkConsoleAIController> controller = data->controller;
    QPointer<Vehicle> vehicle = data->vehicle;
    const PendingConsoleToolCommand toolCommand = data->toolCommand;
    delete data;

    if (!controller) {
        return;
    }
    if (controller->_activeMavlinkToolCommand.toolCallId != toolCommand.toolCallId ||
        controller->_activeMavlinkToolCommand.toolName != toolCommand.toolName) {
        return;
    }

    const bool accepted = ack.result == MAV_RESULT_ACCEPTED && failureCode == Vehicle::MavCmdResultCommandResultOnly;
    QJsonObject result = controller->_toolResultObject(
        toolCommand.toolCallId,
        toolCommand.toolName,
        toolCommand.arguments,
        accepted ? QStringLiteral("ok") : QStringLiteral("error"),
        accepted
            ? controller->tr("Low-privilege MAVLink command was accepted by the vehicle.")
            : controller->tr("Low-privilege MAVLink command was not accepted by the vehicle."),
        toolCommand.command);

    QJsonObject dataObject;
    dataObject.insert(QStringLiteral("ackCommand"), static_cast<int>(ack.command));
    dataObject.insert(QStringLiteral("ackResult"), mavResultText(static_cast<MAV_RESULT>(ack.result)));
    dataObject.insert(QStringLiteral("failureCode"), mavCommandFailureText(failureCode));
    dataObject.insert(QStringLiteral("componentId"), compId);
    dataObject.insert(QStringLiteral("progress"), static_cast<int>(ack.progress));

    if (accepted && toolCommand.executionKind == ToolExecutionSetMessageInterval && toolCommand.ttlSeconds > 0) {
        dataObject.insert(QStringLiteral("restoreScheduled"), true);
        dataObject.insert(QStringLiteral("restoreIntervalUsec"), 0);
        dataObject.insert(QStringLiteral("ttlSeconds"), toolCommand.ttlSeconds);

        const int componentId = toolCommand.componentId;
        const int messageId = toolCommand.messageId;
        const int ttlMsec = toolCommand.ttlSeconds * 1000;
        QTimer::singleShot(ttlMsec, controller, [vehicle, componentId, messageId] {
            if (vehicle) {
                vehicle->sendMavCommand(
                    componentId,
                    MAV_CMD_SET_MESSAGE_INTERVAL,
                    false,
                    static_cast<float>(messageId),
                    0.0f);
            }
        });
    } else {
        dataObject.insert(QStringLiteral("restoreScheduled"), false);
    }

    result.insert(QStringLiteral("data"), dataObject);
    controller->_finishActiveMavlinkToolCommand(toolCommand, result);
}

void MAVLinkConsoleAIController::_finishToolExecution()
{
    if (_automaticToolExecution) {
        const QString diagnosticResults = QString::fromUtf8(QJsonDocument(_pendingToolResultMessages).toJson(QJsonDocument::Compact));
        _pendingMessages.append(QJsonObject{
            { QStringLiteral("role"), QStringLiteral("user") },
            { QStringLiteral("content"), tr("Automatic QGroundControl read-only diagnostic results JSON:\n%1\n\nUse these results as primary evidence. If a command output is empty or timed out, say it is unavailable rather than guessing.").arg(diagnosticResults) }
        });
        _pendingToolResultMessages = QJsonArray();
        _automaticToolExecution = false;

        if (!_postChatRequest(_pendingMessages, false)) {
            _clearPendingChatState();
            _setBusy(false);
        }
        return;
    }

    for (const QJsonValue &toolResultMessage : _pendingToolResultMessages) {
        _pendingMessages.append(toolResultMessage);
    }
    _pendingToolResultMessages = QJsonArray();

    if (!_postChatRequest(_pendingMessages, true, QStringLiteral("none"))) {
        _clearPendingChatState();
        _setBusy(false);
    }
}

void MAVLinkConsoleAIController::_clearConsoleToolExecution(bool sendClose)
{
    _consoleCommandTimer.stop();
    _pendingConsoleToolCommands.clear();
    _pendingToolResultMessages = QJsonArray();
    _activeConsoleOutput.clear();
    _activeConsoleOutputTruncated = false;
    _activeConsoleToolCommand = PendingConsoleToolCommand();
    _activeMavlinkToolCommand = PendingConsoleToolCommand();
    _automaticToolExecution = false;
    _clearPendingApproval();
    if (_consoleDataConnection) {
        (void) disconnect(_consoleDataConnection);
        _consoleDataConnection = QMetaObject::Connection();
    }
    if (sendClose) {
        _sendSerialData(QByteArray(), true);
    }
}

void MAVLinkConsoleAIController::_sendSerialData(const QByteArray &data, bool close)
{
    Vehicle *vehicle = MultiVehicleManager::instance()->activeVehicle();
    if (!vehicle) {
        return;
    }

    SharedLinkInterfacePtr sharedLink = vehicle->vehicleLinkManager()->primaryLink().lock();
    if (!sharedLink) {
        return;
    }

    QByteArray output(data);
    do {
        QByteArray chunk(output.left(MAVLINK_MSG_SERIAL_CONTROL_FIELD_DATA_LEN));
        const int dataSize = chunk.size();
        (void) chunk.append(MAVLINK_MSG_SERIAL_CONTROL_FIELD_DATA_LEN - chunk.size(), '\0');

        const uint8_t flags = close ? 0 : SERIAL_CONTROL_FLAG_EXCLUSIVE | SERIAL_CONTROL_FLAG_RESPOND | SERIAL_CONTROL_FLAG_MULTI;

        mavlink_message_t msg;
        (void) mavlink_msg_serial_control_pack_chan(
            MAVLinkProtocol::instance()->getSystemId(),
            MAVLinkProtocol::getComponentId(),
            sharedLink->mavlinkChannel(),
            &msg,
            SERIAL_CONTROL_DEV_SHELL,
            flags,
            0,
            0,
            dataSize,
            reinterpret_cast<uint8_t*>(chunk.data()),
            vehicle->id(),
            vehicle->defaultComponentId()
        );

        (void) vehicle->sendMessageOnLinkThreadSafe(sharedLink.get(), msg);
        (void) output.remove(0, chunk.size());
    } while (!output.isEmpty());
}

QString MAVLinkConsoleAIController::_cleanConsoleOutput(const QByteArray &output, bool *truncated) const
{
    if (truncated) {
        *truncated = *truncated || (output.size() > kConsoleCommandOutputMaxChars);
    }

    QByteArray clippedOutput = output;
    if (clippedOutput.size() > kConsoleCommandOutputMaxChars) {
        clippedOutput = clippedOutput.right(kConsoleCommandOutputMaxChars);
    }

    QString text = QString::fromUtf8(clippedOutput);
    text.remove(QChar::Null);
    static const QRegularExpression ansiRegex(QStringLiteral("\\x1B\\[[0-9;?]*[ -/]*[@-~]"));
    text.remove(ansiRegex);
    return text.trimmed();
}

bool MAVLinkConsoleAIController::_answerContainsInternalToolMarkup(const QString &answer) const
{
    const QString trimmedAnswer = answer.trimmed();
    if (trimmedAnswer.isEmpty()) {
        return false;
    }

    static const QRegularExpression dsmlTagRegex(QStringLiteral("<\\s*\\|\\s*DSML\\s*\\|"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression pipeTagRegex(QStringLiteral("<\\s*\\|"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression toolCallRegex(QStringLiteral("\\btool_calls?\\b"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression invokeRegex(QStringLiteral("\\binvoke\\s+name\\s*="), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression parameterRegex(QStringLiteral("\\bparameter\\s+name\\s*="), QRegularExpression::CaseInsensitiveOption);

    if (trimmedAnswer.contains(dsmlTagRegex)) {
        return true;
    }

    const bool hasToolCallText = trimmedAnswer.contains(toolCallRegex);
    const bool hasPipeTag = trimmedAnswer.contains(pipeTagRegex);
    const bool hasInvokeMarkup = trimmedAnswer.contains(invokeRegex);
    const bool hasParameterMarkup = trimmedAnswer.contains(parameterRegex);

    return (hasToolCallText && (hasPipeTag || hasInvokeMarkup || hasParameterMarkup))
        || (hasInvokeMarkup && hasParameterMarkup);
}

void MAVLinkConsoleAIController::_clearPendingChatState()
{
    _pendingMessages = QJsonArray();
    _pendingToolResultMessages = QJsonArray();
    _pendingConsoleToolCommands.clear();
    _activeConsoleToolCommand = PendingConsoleToolCommand();
    _activeMavlinkToolCommand = PendingConsoleToolCommand();
    _activeConsoleOutput.clear();
    _activeConsoleOutputTruncated = false;
    _remainingToolRounds = 0;
    _remainingConsoleCommands = 0;
    _remainingLowPrivilegeMavlinkCommands = 0;
    _pendingToolChoice.clear();
    _automaticToolExecution = false;
    _internalToolMarkupRetryUsed = false;
    _clearPendingApproval();
}

void MAVLinkConsoleAIController::_oauthDeviceAuthorizationFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply || (reply != _oauthReply)) {
        if (reply) {
            reply->deleteLater();
        }
        return;
    }

    const QNetworkReply::NetworkError networkError = reply->error();
    const int httpStatusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray payload = reply->readAll();
    const QString networkErrorText = _formatNetworkErrorText(networkError, reply->url(), reply->errorString(), payload, httpStatusCode);
    _clearOAuthReply(false);

    if (networkError != QNetworkReply::NoError) {
        _setOAuthBusy(false);
        _failOAuth(networkErrorText);
        return;
    }

    QJsonParseError parseError{};
    const QJsonDocument responseDocument = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !responseDocument.isObject()) {
        _setOAuthBusy(false);
        _failOAuth(tr("OAuth device authorization response was not valid JSON."));
        return;
    }

    const QJsonObject responseObject = responseDocument.object();
    const QString error = responseObject.value(QStringLiteral("error")).toString();
    if (!error.isEmpty()) {
        _setOAuthBusy(false);
        _failOAuth(responseObject.value(QStringLiteral("error_description")).toString(error));
        return;
    }

    _oauthDeviceCode = responseObject.value(QStringLiteral("device_code")).toString();
    _oauthUserCode = responseObject.value(QStringLiteral("user_code")).toString();
    _oauthVerificationUri = responseObject.value(QStringLiteral("verification_uri")).toString();
    if (_oauthVerificationUri.isEmpty()) {
        _oauthVerificationUri = responseObject.value(QStringLiteral("verification_url")).toString();
    }
    _oauthVerificationUriComplete = responseObject.value(QStringLiteral("verification_uri_complete")).toString();
    _oauthMessage = responseObject.value(QStringLiteral("message")).toString();

    if (_oauthDeviceCode.isEmpty() || _oauthUserCode.isEmpty() || _oauthVerificationUri.isEmpty()) {
        _setOAuthBusy(false);
        _failOAuth(tr("OAuth device authorization response did not include device_code, user_code, or verification_uri."));
        return;
    }

    const int intervalSecs = qMax(1, responseObject.value(QStringLiteral("interval")).toInt(5));
    _oauthPollIntervalMsec = intervalSecs * 1000;
    const int expiresInSecs = qMax(1, responseObject.value(QStringLiteral("expires_in")).toInt(600));
    _oauthDeviceCodeExpiresAtUtc = QDateTime::currentDateTimeUtc().addSecs(expiresInSecs);
    _setOAuthStatusText(tr("Open the authorization page and enter the user code."));
    emit oauthAuthorizationChanged();

    _oauthPollTimer.start(qMin(_oauthPollIntervalMsec, expiresInSecs * 1000));
}

void MAVLinkConsoleAIController::_pollOAuthToken()
{
    if (_oauthDeviceCode.isEmpty()) {
        _setOAuthBusy(false);
        _failOAuth(tr("OAuth device code is not available."));
        return;
    }
    if (_oauthDeviceCodeExpiresAtUtc.isValid() && (QDateTime::currentDateTimeUtc() >= _oauthDeviceCodeExpiresAtUtc)) {
        _setOAuthBusy(false);
        _failOAuth(tr("OAuth user code expired. Start authorization again."));
        return;
    }

    AIConsoleSettings *settings = SettingsManager::instance()->aiConsoleSettings();
    if (!settings) {
        _setOAuthBusy(false);
        _failOAuth(tr("AI console settings are not available."));
        return;
    }

    const QUrl url = QUrl::fromUserInput(settings->oauthTokenUrl()->rawValueString().trimmed());
    if (!url.isValid()) {
        _setOAuthBusy(false);
        _failOAuth(tr("The OAuth token URL is invalid."));
        return;
    }
    QString tlsErrorText;
    if (!_checkTlsAvailable(url, &tlsErrorText)) {
        _setOAuthBusy(false);
        _failOAuth(tlsErrorText);
        return;
    }

    const QList<QPair<QString, QString>> fields{
        { QStringLiteral("grant_type"), QStringLiteral("urn:ietf:params:oauth:grant-type:device_code") },
        { QStringLiteral("device_code"), _oauthDeviceCode },
        { QStringLiteral("client_id"), settings->oauthClientId()->rawValueString().trimmed() }
    };

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    request.setRawHeader(QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json"));
    request.setRawHeader(QByteArrayLiteral("User-Agent"), QByteArrayLiteral("QGroundControl-MAVLinkConsoleAI"));

    _setOAuthStatusText(tr("Waiting for OAuth authorization..."));
    _oauthReply = _networkManager.post(request, _formData(fields));
    (void) connect(_oauthReply, &QNetworkReply::finished, this, &MAVLinkConsoleAIController::_oauthTokenPollFinished);
}

void MAVLinkConsoleAIController::_oauthTokenPollFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply || (reply != _oauthReply)) {
        if (reply) {
            reply->deleteLater();
        }
        return;
    }

    const QNetworkReply::NetworkError networkError = reply->error();
    const int httpStatusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray payload = reply->readAll();
    const QString networkErrorText = _formatNetworkErrorText(networkError, reply->url(), reply->errorString(), payload, httpStatusCode);
    _clearOAuthReply(false);

    QJsonParseError parseError{};
    const QJsonDocument responseDocument = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !responseDocument.isObject()) {
        _setOAuthBusy(false);
        _failOAuth(networkError == QNetworkReply::NoError ? tr("OAuth token response was not valid JSON.") : networkErrorText);
        return;
    }

    const QJsonObject responseObject = responseDocument.object();
    const QString error = responseObject.value(QStringLiteral("error")).toString();
    if (!error.isEmpty()) {
        if (error == QStringLiteral("authorization_pending")) {
            _setOAuthStatusText(tr("Waiting for OAuth authorization..."));
            _oauthPollTimer.start(_oauthPollIntervalMsec);
            return;
        }
        if (error == QStringLiteral("slow_down")) {
            _oauthPollIntervalMsec += 5000;
            _setOAuthStatusText(tr("OAuth provider requested slower polling."));
            _oauthPollTimer.start(_oauthPollIntervalMsec);
            return;
        }

        _setOAuthBusy(false);
        _failOAuth(responseObject.value(QStringLiteral("error_description")).toString(error));
        return;
    }

    if (networkError != QNetworkReply::NoError) {
        _setOAuthBusy(false);
        _failOAuth(networkErrorText);
        return;
    }

    const QString accessToken = responseObject.value(QStringLiteral("access_token")).toString();
    if (accessToken.isEmpty()) {
        _setOAuthBusy(false);
        _failOAuth(tr("OAuth token response did not include an access token."));
        return;
    }

    AIConsoleSettings *settings = SettingsManager::instance()->aiConsoleSettings();
    if (!settings) {
        _setOAuthBusy(false);
        _failOAuth(tr("AI console settings are not available."));
        return;
    }

    settings->oauthAccessToken()->setRawValue(accessToken);
    const QString refreshToken = responseObject.value(QStringLiteral("refresh_token")).toString();
    if (!refreshToken.isEmpty()) {
        settings->oauthRefreshToken()->setRawValue(refreshToken);
    }

    const int expiresInSecs = responseObject.value(QStringLiteral("expires_in")).toInt(0);
    if (expiresInSecs > 0) {
        const qint64 expiresAtSecs = QDateTime::currentDateTimeUtc().addSecs(expiresInSecs).toSecsSinceEpoch();
        settings->oauthTokenExpiresAtUtc()->setRawValue(QString::number(expiresAtSecs));
    } else {
        settings->oauthTokenExpiresAtUtc()->setRawValue(QString());
    }

    _setOAuthBusy(false);
    _setOAuthStatusText(tr("OAuth authorization complete."));
    emit configuredChanged();
    emit oauthAuthorizedChanged();
}

bool MAVLinkConsoleAIController::_oauthSelected() const
{
    AIConsoleSettings *settings = SettingsManager::instance()->aiConsoleSettings();
    return settings && (settings->authMethod()->rawValue().toInt() == kAuthMethodOAuthDevice);
}

bool MAVLinkConsoleAIController::_oauthTokenUsable() const
{
    AIConsoleSettings *settings = SettingsManager::instance()->aiConsoleSettings();
    if (!settings || settings->oauthAccessToken()->rawValueString().trimmed().isEmpty()) {
        return false;
    }

    const QString expiresAtText = settings->oauthTokenExpiresAtUtc()->rawValueString().trimmed();
    if (expiresAtText.isEmpty()) {
        return true;
    }

    bool ok = false;
    const qint64 expiresAtSecs = expiresAtText.toLongLong(&ok);
    if (!ok) {
        return false;
    }

    return QDateTime::currentDateTimeUtc().toSecsSinceEpoch() < (expiresAtSecs - 60);
}

bool MAVLinkConsoleAIController::_checkTlsAvailable(const QUrl &url, QString *errorText) const
{
    if (url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0) {
        return true;
    }

    if (QSslSocket::supportsSsl()) {
        return true;
    }

    if (errorText) {
        *errorText = _tlsUnavailableText();
    }
    return false;
}

QString MAVLinkConsoleAIController::_tlsUnavailableText() const
{
    const QString sslBuildVersion = QSslSocket::sslLibraryBuildVersionString();
    const QString sslRuntimeVersion = QSslSocket::sslLibraryVersionString();
    QStringList details;
    if (!sslBuildVersion.isEmpty()) {
        details << tr("Qt SSL build: %1").arg(sslBuildVersion);
    }
    if (!sslRuntimeVersion.isEmpty()) {
        details << tr("loaded SSL runtime: %1").arg(sslRuntimeVersion);
    }
    if (details.isEmpty()) {
        details << tr("no SSL runtime was loaded");
    }

    return tr("TLS is unavailable. Qt could not initialize its OpenSSL backend (%1). "
              "Start QGroundControl with an OpenSSL 3 runtime available in LD_LIBRARY_PATH.")
        .arg(details.join(QStringLiteral("; ")));
}

QString MAVLinkConsoleAIController::_formatNetworkErrorText(QNetworkReply::NetworkError networkError, const QUrl &url, const QString &errorText, const QByteArray &payload, int httpStatusCode) const
{
    const QString trimmedError = errorText.trimmed();
    if (trimmedError.contains(QStringLiteral("TLS"), Qt::CaseInsensitive)
            || trimmedError.contains(QStringLiteral("SSL"), Qt::CaseInsensitive)) {
        return _tlsUnavailableText();
    }

    const QString host = url.host().isEmpty() ? tr("the configured host") : url.host();
    const QString original = trimmedError.isEmpty() ? tr("Network request failed.") : trimmedError;

    QString message;
    switch (networkError) {
    case QNetworkReply::HostNotFoundError:
        message = tr("Cannot resolve AI endpoint host \"%1\". Check that the endpoint URL is correct. "
                  "For DeepSeek, use an OpenAI-compatible chat completions URL such as "
                  "https://api.deepseek.com/chat/completions or https://api.deepseek.com/v1/chat/completions. "
                  "If the URL is correct, check DNS, proxy/VPN, firewall, and internet access. Original error: %2")
            .arg(host, original);
        break;
    case QNetworkReply::TimeoutError:
        message = tr("The AI endpoint \"%1\" did not respond before timeout. Check network quality, proxy/VPN, and endpoint availability. Original error: %2")
            .arg(host, original);
        break;
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
        message = tr("Temporary network failure while contacting AI endpoint \"%1\". Check internet access and retry. Original error: %2")
            .arg(host, original);
        break;
    case QNetworkReply::ProxyNotFoundError:
    case QNetworkReply::ProxyConnectionRefusedError:
    case QNetworkReply::ProxyConnectionClosedError:
    case QNetworkReply::ProxyTimeoutError:
        message = tr("Proxy connection failed while contacting AI endpoint \"%1\". Check system proxy/VPN settings. Original error: %2")
            .arg(host, original);
        break;
    case QNetworkReply::SslHandshakeFailedError:
        message = tr("TLS handshake failed while contacting AI endpoint \"%1\". Check HTTPS endpoint, system time, certificates, and proxy interception. Original error: %2")
            .arg(host, original);
        break;
    default:
        message = original;
        break;
    }

    QStringList details;
    if (httpStatusCode > 0) {
        details << tr("HTTP status: %1").arg(httpStatusCode);
    }
    const QString responseText = _responseErrorText(payload);
    if (!responseText.isEmpty()) {
        details << tr("response: %1").arg(responseText);
    }
    if (!details.isEmpty()) {
        message = tr("%1 (%2)").arg(message, details.join(QStringLiteral("; ")));
    }
    return message;
}

QString MAVLinkConsoleAIController::_responseErrorText(const QByteArray &payload) const
{
    const QByteArray trimmedPayload = payload.trimmed();
    if (trimmedPayload.isEmpty()) {
        return QString();
    }

    auto normalize = [](QString text) {
        text = text.trimmed();
        static const QRegularExpression whitespaceRegex(QStringLiteral("\\s+"));
        text.replace(whitespaceRegex, QStringLiteral(" "));
        if (text.size() > kNetworkErrorBodyMaxChars) {
            text = text.left(kNetworkErrorBodyMaxChars) + QStringLiteral("...");
        }
        return text;
    };

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(trimmedPayload, &parseError);
    if (parseError.error == QJsonParseError::NoError) {
        if (document.isObject()) {
            const QJsonObject object = document.object();
            QStringList parts;
            const QJsonValue errorValue = object.value(QStringLiteral("error"));
            if (errorValue.isObject()) {
                const QJsonObject errorObject = errorValue.toObject();
                const QString errorMessage = errorObject.value(QStringLiteral("message")).toString().trimmed();
                const QString errorType = errorObject.value(QStringLiteral("type")).toString().trimmed();
                const QString errorCode = errorObject.value(QStringLiteral("code")).toVariant().toString().trimmed();
                if (!errorMessage.isEmpty()) {
                    parts << errorMessage;
                }
                if (!errorType.isEmpty()) {
                    parts << tr("type=%1").arg(errorType);
                }
                if (!errorCode.isEmpty()) {
                    parts << tr("code=%1").arg(errorCode);
                }
            } else if (errorValue.isString()) {
                parts << errorValue.toString().trimmed();
            }

            const QString errorDescription = object.value(QStringLiteral("error_description")).toString().trimmed();
            if (!errorDescription.isEmpty() && !parts.contains(errorDescription)) {
                parts << errorDescription;
            }
            const QString message = object.value(QStringLiteral("message")).toString().trimmed();
            if (!message.isEmpty() && !parts.contains(message)) {
                parts << message;
            }
            if (!parts.isEmpty()) {
                return normalize(parts.join(QStringLiteral("; ")));
            }

            return normalize(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
        }

        return normalize(QString::fromUtf8(document.toJson(QJsonDocument::Compact)));
    }

    return normalize(QString::fromUtf8(trimmedPayload));
}

bool MAVLinkConsoleAIController::_shouldRetryNetworkError(QNetworkReply::NetworkError networkError) const
{
    switch (networkError) {
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::TimeoutError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::ProxyTimeoutError:
    case QNetworkReply::ServiceUnavailableError:
        return true;
    default:
        return false;
    }
}

QString MAVLinkConsoleAIController::_authorizationHeaderValue(AIConsoleSettings *settings) const
{
    if (!settings) {
        return QString();
    }

    if (settings->authMethod()->rawValue().toInt() == kAuthMethodOAuthDevice) {
        return _oauthTokenUsable()
            ? QStringLiteral("Bearer %1").arg(settings->oauthAccessToken()->rawValueString().trimmed())
            : QString();
    }

    const QString apiKey = settings->apiKey()->rawValueString().trimmed();
    return apiKey.isEmpty() ? QString() : QStringLiteral("Bearer %1").arg(apiKey);
}

QByteArray MAVLinkConsoleAIController::_formData(const QList<QPair<QString, QString>> &fields) const
{
    QUrlQuery query;
    for (const QPair<QString, QString> &field : fields) {
        query.addQueryItem(field.first, field.second);
    }
    return query.toString(QUrl::FullyEncoded).toUtf8();
}

QJsonObject MAVLinkConsoleAIController::_buildVehicleSnapshot(Vehicle *vehicle) const
{
    QJsonObject snapshot;
    snapshot.insert(QStringLiteral("source"), QStringLiteral("QGroundControl active vehicle telemetry"));
    snapshot.insert(QStringLiteral("timestampUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    snapshot.insert(QStringLiteral("activeVehicle"), vehicle != nullptr);

    if (!vehicle) {
        snapshot.insert(QStringLiteral("note"), QStringLiteral("No active vehicle is connected. Only recent MAVLink Console output may be available."));
        snapshot.insert(QStringLiteral("core"), QJsonObject{
            { QStringLiteral("available"), false }
        });
        snapshot.insert(QStringLiteral("sysStatusSensorInfo"), QJsonObject{
            { QStringLiteral("available"), false }
        });
        snapshot.insert(QStringLiteral("healthAndArmingCheckReport"), QJsonObject{
            { QStringLiteral("available"), false }
        });
        snapshot.insert(QStringLiteral("linkStatus"), QJsonObject{
            { QStringLiteral("available"), false }
        });
        snapshot.insert(QStringLiteral("factGroups"), QJsonObject());
        snapshot.insert(QStringLiteral("batteries"), QJsonArray());
        return snapshot;
    }

    QJsonObject core;
    core.insert(QStringLiteral("available"), true);
    core.insert(QStringLiteral("vehicleId"), vehicle->id());
    core.insert(QStringLiteral("firmware"), vehicle->firmwareTypeString());
    core.insert(QStringLiteral("vehicleType"), vehicle->vehicleTypeString());
    core.insert(QStringLiteral("armed"), vehicle->armed());
    core.insert(QStringLiteral("flying"), vehicle->flying());
    core.insert(QStringLiteral("landing"), vehicle->landing());
    core.insert(QStringLiteral("flightMode"), vehicle->flightMode());
    core.insert(QStringLiteral("readyToFlyAvailable"), vehicle->readyToFlyAvailable());
    core.insert(QStringLiteral("readyToFly"), vehicle->readyToFly());
    core.insert(QStringLiteral("prearmError"), vehicle->prearmError());
    core.insert(QStringLiteral("allSensorsHealthy"), vehicle->allSensorsHealthy());
    core.insert(QStringLiteral("requiresGpsFix"), vehicle->requiresGpsFix());
    core.insert(QStringLiteral("communicationLost"), vehicle->vehicleLinkManager()->communicationLost());
    core.insert(QStringLiteral("mavlinkSentCount"), QJsonValue::fromVariant(QVariant::fromValue(vehicle->mavlinkSentCount())));
    core.insert(QStringLiteral("mavlinkReceivedCount"), QJsonValue::fromVariant(QVariant::fromValue(vehicle->mavlinkReceivedCount())));
    core.insert(QStringLiteral("mavlinkLossCount"), QJsonValue::fromVariant(QVariant::fromValue(vehicle->mavlinkLossCount())));
    core.insert(QStringLiteral("mavlinkLossPercent"), vehicle->mavlinkLossPercent());
    core.insert(QStringLiteral("coordinate"), _coordinateToJson(vehicle->coordinate()));
    core.insert(QStringLiteral("homePosition"), _coordinateToJson(vehicle->homePosition()));
    snapshot.insert(QStringLiteral("core"), core);
    snapshot.insert(QStringLiteral("sysStatusSensorInfo"), _buildSysStatusSensorInfoJson(vehicle));
    snapshot.insert(QStringLiteral("healthAndArmingCheckReport"), _buildHealthAndArmingCheckReportJson(vehicle));
    snapshot.insert(QStringLiteral("linkStatus"), _buildLinkStatusJson(vehicle));

    QJsonObject factGroups;
    factGroups.insert(QStringLiteral("vehicle"), _factGroupToJson(vehicle));
    for (const QString &groupName : vehicle->factGroupNames()) {
        factGroups.insert(groupName, _factGroupToJson(vehicle->getFactGroup(groupName)));
    }
    snapshot.insert(QStringLiteral("factGroups"), factGroups);

    QJsonArray batteries;
    QmlObjectListModel *batteryModel = vehicle->batteries();
    for (int i = 0; i < batteryModel->count(); ++i) {
        if (const FactGroup *batteryGroup = qobject_cast<const FactGroup*>(batteryModel->get(i))) {
            batteries.append(_factGroupToJson(batteryGroup));
        }
    }
    snapshot.insert(QStringLiteral("batteries"), batteries);

    return snapshot;
}

QJsonObject MAVLinkConsoleAIController::_coordinateToJson(const QGeoCoordinate &coordinate) const
{
    QJsonObject object;
    object.insert(QStringLiteral("valid"), coordinate.isValid());
    if (coordinate.isValid()) {
        object.insert(QStringLiteral("latitude"), coordinate.latitude());
        object.insert(QStringLiteral("longitude"), coordinate.longitude());
        if (!qIsNaN(coordinate.altitude())) {
            object.insert(QStringLiteral("altitude"), coordinate.altitude());
        }
    }
    return object;
}

QJsonObject MAVLinkConsoleAIController::_buildSysStatusSensorInfoJson(Vehicle *vehicle) const
{
    QJsonObject object;
    if (!vehicle) {
        object.insert(QStringLiteral("available"), false);
        return object;
    }

    QObject *sensorInfo = vehicle->sysStatusSensorInfo();
    if (!sensorInfo) {
        object.insert(QStringLiteral("available"), false);
        return object;
    }

    const QStringList sensorNames = sensorInfo->property("sensorNames").toStringList();
    const QStringList sensorStatus = sensorInfo->property("sensorStatus").toStringList();
    QJsonArray sensors;
    const int count = qMin(sensorNames.size(), sensorStatus.size());
    for (int i = 0; i < count; ++i) {
        sensors.append(QJsonObject{
            { QStringLiteral("name"), sensorNames.at(i) },
            { QStringLiteral("status"), sensorStatus.at(i) }
        });
    }

    object.insert(QStringLiteral("available"), true);
    object.insert(QStringLiteral("sensors"), sensors);
    return object;
}

QJsonObject MAVLinkConsoleAIController::_buildHealthAndArmingCheckReportJson(Vehicle *vehicle) const
{
    QJsonObject object;
    if (!vehicle || !vehicle->healthAndArmingCheckReport()) {
        object.insert(QStringLiteral("available"), false);
        return object;
    }

    HealthAndArmingCheckReport *report = vehicle->healthAndArmingCheckReport();
    object.insert(QStringLiteral("available"), true);
    object.insert(QStringLiteral("supported"), report->supported());
    object.insert(QStringLiteral("canArm"), report->canArm());
    object.insert(QStringLiteral("canTakeoff"), report->canTakeoff());
    object.insert(QStringLiteral("canStartMission"), report->canStartMission());
    object.insert(QStringLiteral("hasWarningsOrErrors"), report->hasWarningsOrErrors());
    object.insert(QStringLiteral("gpsState"), report->gpsState());

    QJsonArray problems;
    if (QmlObjectListModel *problemModel = report->problemsForCurrentMode()) {
        for (int i = 0; i < problemModel->count(); ++i) {
            const QObject *problem = problemModel->get(i);
            if (!problem) {
                continue;
            }
            problems.append(QJsonObject{
                { QStringLiteral("message"), problem->property("message").toString() },
                { QStringLiteral("description"), problem->property("description").toString() },
                { QStringLiteral("severity"), problem->property("severity").toString() }
            });
        }
    }
    object.insert(QStringLiteral("problemsForCurrentMode"), problems);
    return object;
}

QJsonObject MAVLinkConsoleAIController::_buildLinkStatusJson(Vehicle *vehicle) const
{
    QJsonObject object;
    if (!vehicle) {
        object.insert(QStringLiteral("available"), false);
        return object;
    }

    object.insert(QStringLiteral("available"), true);
    object.insert(QStringLiteral("communicationLost"), vehicle->vehicleLinkManager()->communicationLost());
    object.insert(QStringLiteral("mavlinkSentCount"), QJsonValue::fromVariant(QVariant::fromValue(vehicle->mavlinkSentCount())));
    object.insert(QStringLiteral("mavlinkReceivedCount"), QJsonValue::fromVariant(QVariant::fromValue(vehicle->mavlinkReceivedCount())));
    object.insert(QStringLiteral("mavlinkLossCount"), QJsonValue::fromVariant(QVariant::fromValue(vehicle->mavlinkLossCount())));
    object.insert(QStringLiteral("mavlinkLossPercent"), vehicle->mavlinkLossPercent());
    object.insert(QStringLiteral("messagesReceived"), QJsonValue::fromVariant(QVariant::fromValue(vehicle->messagesReceived())));
    object.insert(QStringLiteral("messagesSent"), QJsonValue::fromVariant(QVariant::fromValue(vehicle->messagesSent())));
    object.insert(QStringLiteral("messagesLost"), QJsonValue::fromVariant(QVariant::fromValue(vehicle->messagesLost())));
    object.insert(QStringLiteral("telemetryRRSSI"), vehicle->telemetryRRSSI());
    object.insert(QStringLiteral("telemetryLRSSI"), vehicle->telemetryLRSSI());
    object.insert(QStringLiteral("telemetryRXErrors"), QJsonValue::fromVariant(QVariant::fromValue(vehicle->telemetryRXErrors())));
    object.insert(QStringLiteral("telemetryFixed"), QJsonValue::fromVariant(QVariant::fromValue(vehicle->telemetryFixed())));
    object.insert(QStringLiteral("telemetryTXBuffer"), QJsonValue::fromVariant(QVariant::fromValue(vehicle->telemetryTXBuffer())));
    object.insert(QStringLiteral("telemetryLNoise"), vehicle->telemetryLNoise());
    object.insert(QStringLiteral("telemetryRNoise"), vehicle->telemetryRNoise());
    object.insert(QStringLiteral("mavlinkSigning"), vehicle->mavlinkSigning());
    return object;
}

QJsonObject MAVLinkConsoleAIController::_factGroupToJson(const FactGroup *factGroup) const
{
    QJsonObject object;
    if (!factGroup) {
        object.insert(QStringLiteral("available"), false);
        return object;
    }

    object.insert(QStringLiteral("available"), true);
    object.insert(QStringLiteral("telemetryAvailable"), factGroup->telemetryAvailable());

    QJsonObject facts;
    for (const QString &factName : factGroup->factNames()) {
        facts.insert(factName, _factToJson(factGroup->getFact(factName)));
    }
    object.insert(QStringLiteral("facts"), facts);

    return object;
}

QJsonObject MAVLinkConsoleAIController::_factToJson(const Fact *fact) const
{
    QJsonObject object;
    if (!fact) {
        object.insert(QStringLiteral("available"), false);
        return object;
    }

    object.insert(QStringLiteral("available"), true);
    object.insert(QStringLiteral("name"), fact->name());
    object.insert(QStringLiteral("componentId"), fact->componentId());
    object.insert(QStringLiteral("value"), _variantToJson(fact->cookedValue()));
    object.insert(QStringLiteral("rawValue"), _variantToJson(fact->rawValue()));
    object.insert(QStringLiteral("valueString"), fact->cookedValueString());
    object.insert(QStringLiteral("rawValueString"), fact->rawValueString());
    object.insert(QStringLiteral("rawValueStringFullPrecision"), fact->rawValueStringFullPrecision());
    object.insert(QStringLiteral("units"), fact->cookedUnits());
    object.insert(QStringLiteral("type"), static_cast<int>(fact->type()));
    object.insert(QStringLiteral("shortDescription"), fact->shortDescription());
    object.insert(QStringLiteral("longDescription"), fact->longDescription());
    object.insert(QStringLiteral("category"), fact->category());
    object.insert(QStringLiteral("group"), fact->group());
    object.insert(QStringLiteral("decimalPlaces"), fact->decimalPlaces());
    object.insert(QStringLiteral("defaultValueAvailable"), fact->defaultValueAvailable());
    if (fact->defaultValueAvailable()) {
        object.insert(QStringLiteral("defaultValue"), _variantToJson(fact->cookedDefaultValue()));
        object.insert(QStringLiteral("defaultValueString"), fact->cookedDefaultValueString());
        object.insert(QStringLiteral("valueEqualsDefault"), fact->valueEqualsDefault());
    }
    object.insert(QStringLiteral("min"), _variantToJson(fact->cookedMin()));
    object.insert(QStringLiteral("minString"), fact->cookedMinString());
    object.insert(QStringLiteral("minIsDefaultForType"), fact->minIsDefaultForType());
    object.insert(QStringLiteral("max"), _variantToJson(fact->cookedMax()));
    object.insert(QStringLiteral("maxString"), fact->cookedMaxString());
    object.insert(QStringLiteral("maxIsDefaultForType"), fact->maxIsDefaultForType());
    object.insert(QStringLiteral("increment"), fact->cookedIncrement());
    object.insert(QStringLiteral("readOnly"), fact->readOnly());
    object.insert(QStringLiteral("writeOnly"), fact->writeOnly());
    object.insert(QStringLiteral("volatileValue"), fact->volatileValue());
    object.insert(QStringLiteral("vehicleRebootRequired"), fact->vehicleRebootRequired());
    object.insert(QStringLiteral("qgcRebootRequired"), fact->qgcRebootRequired());
    object.insert(QStringLiteral("enumStrings"), QJsonArray::fromStringList(fact->enumStrings()));
    object.insert(QStringLiteral("enumValues"), QJsonArray::fromVariantList(fact->enumValues()));
    object.insert(QStringLiteral("bitmaskStrings"), QJsonArray::fromStringList(fact->bitmaskStrings()));
    object.insert(QStringLiteral("bitmaskValues"), QJsonArray::fromVariantList(fact->bitmaskValues()));
    object.insert(QStringLiteral("selectedBitmaskStrings"), QJsonArray::fromStringList(fact->selectedBitmaskStrings()));
    return object;
}

QJsonObject MAVLinkConsoleAIController::_parameterToJson(const Fact *fact) const
{
    QJsonObject object = _factToJson(fact);
    object.insert(QStringLiteral("kind"), QStringLiteral("px4Parameter"));
    if (fact) {
        object.insert(QStringLiteral("parameterName"), fact->name());
        object.insert(QStringLiteral("componentId"), fact->componentId());
    }
    return object;
}

QJsonObject MAVLinkConsoleAIController::_mavlinkMessageToJson(const mavlink_message_t &message) const
{
    auto uint64ToString = [](quint64 value) {
        return QString::number(value);
    };
    auto bytesToHex = [](const uint8_t *bytes, int length) {
        return QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(bytes), length).toHex());
    };

    QJsonObject object;
    object.insert(QStringLiteral("msgid"), static_cast<int>(message.msgid));
    object.insert(QStringLiteral("sysid"), static_cast<int>(message.sysid));
    object.insert(QStringLiteral("compid"), static_cast<int>(message.compid));
    object.insert(QStringLiteral("seq"), static_cast<int>(message.seq));
    object.insert(QStringLiteral("len"), static_cast<int>(message.len));
    object.insert(QStringLiteral("payloadHex"), QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(_MAV_PAYLOAD(&message)), message.len).toHex()));

    QJsonObject decoded;
    switch (message.msgid) {
    case MAVLINK_MSG_ID_HEARTBEAT: {
        mavlink_heartbeat_t heartbeat{};
        mavlink_msg_heartbeat_decode(&message, &heartbeat);
        decoded.insert(QStringLiteral("type"), heartbeat.type);
        decoded.insert(QStringLiteral("autopilot"), heartbeat.autopilot);
        decoded.insert(QStringLiteral("baseMode"), heartbeat.base_mode);
        decoded.insert(QStringLiteral("customMode"), QJsonValue::fromVariant(QVariant::fromValue(heartbeat.custom_mode)));
        decoded.insert(QStringLiteral("systemStatus"), heartbeat.system_status);
        decoded.insert(QStringLiteral("mavlinkVersion"), heartbeat.mavlink_version);
        break;
    }
    case MAVLINK_MSG_ID_SYS_STATUS: {
        mavlink_sys_status_t sysStatus{};
        mavlink_msg_sys_status_decode(&message, &sysStatus);
        decoded.insert(QStringLiteral("sensorsPresent"), QString::number(sysStatus.onboard_control_sensors_present));
        decoded.insert(QStringLiteral("sensorsEnabled"), QString::number(sysStatus.onboard_control_sensors_enabled));
        decoded.insert(QStringLiteral("sensorsHealth"), QString::number(sysStatus.onboard_control_sensors_health));
        decoded.insert(QStringLiteral("load"), sysStatus.load);
        decoded.insert(QStringLiteral("voltageBattery"), sysStatus.voltage_battery);
        decoded.insert(QStringLiteral("currentBattery"), sysStatus.current_battery);
        decoded.insert(QStringLiteral("batteryRemaining"), sysStatus.battery_remaining);
        decoded.insert(QStringLiteral("dropRateComm"), sysStatus.drop_rate_comm);
        decoded.insert(QStringLiteral("errorsComm"), sysStatus.errors_comm);
        decoded.insert(QStringLiteral("errorsCount1"), sysStatus.errors_count1);
        decoded.insert(QStringLiteral("errorsCount2"), sysStatus.errors_count2);
        decoded.insert(QStringLiteral("errorsCount3"), sysStatus.errors_count3);
        decoded.insert(QStringLiteral("errorsCount4"), sysStatus.errors_count4);
        break;
    }
    case MAVLINK_MSG_ID_SYSTEM_TIME: {
        mavlink_system_time_t systemTime{};
        mavlink_msg_system_time_decode(&message, &systemTime);
        decoded.insert(QStringLiteral("timeUnixUsec"), uint64ToString(systemTime.time_unix_usec));
        decoded.insert(QStringLiteral("timeBootMs"), QJsonValue::fromVariant(QVariant::fromValue(systemTime.time_boot_ms)));
        break;
    }
    case MAVLINK_MSG_ID_GPS_RAW_INT: {
        mavlink_gps_raw_int_t gps{};
        mavlink_msg_gps_raw_int_decode(&message, &gps);
        decoded.insert(QStringLiteral("timeUsec"), uint64ToString(gps.time_usec));
        decoded.insert(QStringLiteral("fixType"), gps.fix_type);
        decoded.insert(QStringLiteral("latE7"), gps.lat);
        decoded.insert(QStringLiteral("lonE7"), gps.lon);
        decoded.insert(QStringLiteral("latitude"), gps.lat / 10000000.0);
        decoded.insert(QStringLiteral("longitude"), gps.lon / 10000000.0);
        decoded.insert(QStringLiteral("altMm"), gps.alt);
        decoded.insert(QStringLiteral("eph"), gps.eph);
        decoded.insert(QStringLiteral("epv"), gps.epv);
        decoded.insert(QStringLiteral("vel"), gps.vel);
        decoded.insert(QStringLiteral("cog"), gps.cog);
        decoded.insert(QStringLiteral("satellitesVisible"), gps.satellites_visible);
        break;
    }
    case MAVLINK_MSG_ID_ATTITUDE: {
        mavlink_attitude_t attitude{};
        mavlink_msg_attitude_decode(&message, &attitude);
        decoded.insert(QStringLiteral("timeBootMs"), QJsonValue::fromVariant(QVariant::fromValue(attitude.time_boot_ms)));
        decoded.insert(QStringLiteral("roll"), attitude.roll);
        decoded.insert(QStringLiteral("pitch"), attitude.pitch);
        decoded.insert(QStringLiteral("yaw"), attitude.yaw);
        decoded.insert(QStringLiteral("rollspeed"), attitude.rollspeed);
        decoded.insert(QStringLiteral("pitchspeed"), attitude.pitchspeed);
        decoded.insert(QStringLiteral("yawspeed"), attitude.yawspeed);
        break;
    }
    case MAVLINK_MSG_ID_LOCAL_POSITION_NED: {
        mavlink_local_position_ned_t localPosition{};
        mavlink_msg_local_position_ned_decode(&message, &localPosition);
        decoded.insert(QStringLiteral("timeBootMs"), QJsonValue::fromVariant(QVariant::fromValue(localPosition.time_boot_ms)));
        decoded.insert(QStringLiteral("x"), localPosition.x);
        decoded.insert(QStringLiteral("y"), localPosition.y);
        decoded.insert(QStringLiteral("z"), localPosition.z);
        decoded.insert(QStringLiteral("vx"), localPosition.vx);
        decoded.insert(QStringLiteral("vy"), localPosition.vy);
        decoded.insert(QStringLiteral("vz"), localPosition.vz);
        break;
    }
    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT: {
        mavlink_global_position_int_t globalPosition{};
        mavlink_msg_global_position_int_decode(&message, &globalPosition);
        decoded.insert(QStringLiteral("timeBootMs"), QJsonValue::fromVariant(QVariant::fromValue(globalPosition.time_boot_ms)));
        decoded.insert(QStringLiteral("latE7"), globalPosition.lat);
        decoded.insert(QStringLiteral("lonE7"), globalPosition.lon);
        decoded.insert(QStringLiteral("latitude"), globalPosition.lat / 10000000.0);
        decoded.insert(QStringLiteral("longitude"), globalPosition.lon / 10000000.0);
        decoded.insert(QStringLiteral("altMm"), globalPosition.alt);
        decoded.insert(QStringLiteral("relativeAltMm"), globalPosition.relative_alt);
        decoded.insert(QStringLiteral("vx"), globalPosition.vx);
        decoded.insert(QStringLiteral("vy"), globalPosition.vy);
        decoded.insert(QStringLiteral("vz"), globalPosition.vz);
        decoded.insert(QStringLiteral("hdg"), globalPosition.hdg);
        break;
    }
    case MAVLINK_MSG_ID_HOME_POSITION: {
        mavlink_home_position_t homePosition{};
        mavlink_msg_home_position_decode(&message, &homePosition);
        decoded.insert(QStringLiteral("latitudeE7"), homePosition.latitude);
        decoded.insert(QStringLiteral("longitudeE7"), homePosition.longitude);
        decoded.insert(QStringLiteral("latitude"), homePosition.latitude / 10000000.0);
        decoded.insert(QStringLiteral("longitude"), homePosition.longitude / 10000000.0);
        decoded.insert(QStringLiteral("altitudeMm"), homePosition.altitude);
        decoded.insert(QStringLiteral("x"), homePosition.x);
        decoded.insert(QStringLiteral("y"), homePosition.y);
        decoded.insert(QStringLiteral("z"), homePosition.z);
        QJsonArray q;
        for (int i = 0; i < MAVLINK_MSG_HOME_POSITION_FIELD_Q_LEN; ++i) {
            q.append(homePosition.q[i]);
        }
        decoded.insert(QStringLiteral("q"), q);
        decoded.insert(QStringLiteral("approachX"), homePosition.approach_x);
        decoded.insert(QStringLiteral("approachY"), homePosition.approach_y);
        decoded.insert(QStringLiteral("approachZ"), homePosition.approach_z);
        decoded.insert(QStringLiteral("timeUsec"), uint64ToString(homePosition.time_usec));
        break;
    }
    case MAVLINK_MSG_ID_VFR_HUD: {
        mavlink_vfr_hud_t vfrHud{};
        mavlink_msg_vfr_hud_decode(&message, &vfrHud);
        decoded.insert(QStringLiteral("airspeed"), vfrHud.airspeed);
        decoded.insert(QStringLiteral("groundspeed"), vfrHud.groundspeed);
        decoded.insert(QStringLiteral("heading"), vfrHud.heading);
        decoded.insert(QStringLiteral("throttle"), vfrHud.throttle);
        decoded.insert(QStringLiteral("alt"), vfrHud.alt);
        decoded.insert(QStringLiteral("climb"), vfrHud.climb);
        break;
    }
    case MAVLINK_MSG_ID_EXTENDED_SYS_STATE: {
        mavlink_extended_sys_state_t extendedState{};
        mavlink_msg_extended_sys_state_decode(&message, &extendedState);
        decoded.insert(QStringLiteral("vtolState"), extendedState.vtol_state);
        decoded.insert(QStringLiteral("landedState"), extendedState.landed_state);
        break;
    }
    case MAVLINK_MSG_ID_BATTERY_STATUS: {
        mavlink_battery_status_t batteryStatus{};
        mavlink_msg_battery_status_decode(&message, &batteryStatus);
        decoded.insert(QStringLiteral("id"), batteryStatus.id);
        decoded.insert(QStringLiteral("batteryFunction"), batteryStatus.battery_function);
        decoded.insert(QStringLiteral("type"), batteryStatus.type);
        decoded.insert(QStringLiteral("temperature"), batteryStatus.temperature);
        QJsonArray voltages;
        for (int i = 0; i < MAVLINK_MSG_BATTERY_STATUS_FIELD_VOLTAGES_LEN; ++i) {
            voltages.append(batteryStatus.voltages[i]);
        }
        decoded.insert(QStringLiteral("voltages"), voltages);
        decoded.insert(QStringLiteral("currentBattery"), batteryStatus.current_battery);
        decoded.insert(QStringLiteral("currentConsumed"), batteryStatus.current_consumed);
        decoded.insert(QStringLiteral("energyConsumed"), batteryStatus.energy_consumed);
        decoded.insert(QStringLiteral("batteryRemaining"), batteryStatus.battery_remaining);
        decoded.insert(QStringLiteral("timeRemaining"), batteryStatus.time_remaining);
        decoded.insert(QStringLiteral("chargeState"), batteryStatus.charge_state);
        QJsonArray voltagesExt;
        for (int i = 0; i < MAVLINK_MSG_BATTERY_STATUS_FIELD_VOLTAGES_EXT_LEN; ++i) {
            voltagesExt.append(batteryStatus.voltages_ext[i]);
        }
        decoded.insert(QStringLiteral("voltagesExt"), voltagesExt);
        decoded.insert(QStringLiteral("mode"), batteryStatus.mode);
        decoded.insert(QStringLiteral("faultBitmask"), QString::number(batteryStatus.fault_bitmask));
        break;
    }
    case MAVLINK_MSG_ID_AUTOPILOT_VERSION: {
        mavlink_autopilot_version_t version{};
        mavlink_msg_autopilot_version_decode(&message, &version);
        decoded.insert(QStringLiteral("capabilities"), uint64ToString(version.capabilities));
        decoded.insert(QStringLiteral("flightSwVersion"), QJsonValue::fromVariant(QVariant::fromValue(version.flight_sw_version)));
        decoded.insert(QStringLiteral("middlewareSwVersion"), QJsonValue::fromVariant(QVariant::fromValue(version.middleware_sw_version)));
        decoded.insert(QStringLiteral("osSwVersion"), QJsonValue::fromVariant(QVariant::fromValue(version.os_sw_version)));
        decoded.insert(QStringLiteral("boardVersion"), QJsonValue::fromVariant(QVariant::fromValue(version.board_version)));
        decoded.insert(QStringLiteral("vendorId"), version.vendor_id);
        decoded.insert(QStringLiteral("productId"), version.product_id);
        decoded.insert(QStringLiteral("uid"), uint64ToString(version.uid));
        decoded.insert(QStringLiteral("flightCustomVersionHex"), bytesToHex(version.flight_custom_version, MAVLINK_MSG_AUTOPILOT_VERSION_FIELD_FLIGHT_CUSTOM_VERSION_LEN));
        decoded.insert(QStringLiteral("middlewareCustomVersionHex"), bytesToHex(version.middleware_custom_version, MAVLINK_MSG_AUTOPILOT_VERSION_FIELD_MIDDLEWARE_CUSTOM_VERSION_LEN));
        decoded.insert(QStringLiteral("osCustomVersionHex"), bytesToHex(version.os_custom_version, MAVLINK_MSG_AUTOPILOT_VERSION_FIELD_OS_CUSTOM_VERSION_LEN));
        decoded.insert(QStringLiteral("uid2Hex"), bytesToHex(version.uid2, MAVLINK_MSG_AUTOPILOT_VERSION_FIELD_UID2_LEN));
        break;
    }
    case MAVLINK_MSG_ID_ESTIMATOR_STATUS: {
        mavlink_estimator_status_t estimatorStatus{};
        mavlink_msg_estimator_status_decode(&message, &estimatorStatus);
        decoded.insert(QStringLiteral("timeUsec"), uint64ToString(estimatorStatus.time_usec));
        decoded.insert(QStringLiteral("flags"), estimatorStatus.flags);
        decoded.insert(QStringLiteral("velocityRatio"), estimatorStatus.vel_ratio);
        decoded.insert(QStringLiteral("posHorizRatio"), estimatorStatus.pos_horiz_ratio);
        decoded.insert(QStringLiteral("posVertRatio"), estimatorStatus.pos_vert_ratio);
        decoded.insert(QStringLiteral("magRatio"), estimatorStatus.mag_ratio);
        decoded.insert(QStringLiteral("haglRatio"), estimatorStatus.hagl_ratio);
        decoded.insert(QStringLiteral("tasRatio"), estimatorStatus.tas_ratio);
        decoded.insert(QStringLiteral("posHorizAccuracy"), estimatorStatus.pos_horiz_accuracy);
        decoded.insert(QStringLiteral("posVertAccuracy"), estimatorStatus.pos_vert_accuracy);
        break;
    }
    default:
        break;
    }

    if (!decoded.isEmpty()) {
        object.insert(QStringLiteral("decoded"), decoded);
    }
    return object;
}

QJsonValue MAVLinkConsoleAIController::_variantToJson(const QVariant &value) const
{
    if (!value.isValid() || value.isNull()) {
        return QJsonValue();
    }

    switch (value.metaType().id()) {
    case QMetaType::Float:
    case QMetaType::Double: {
        const double number = value.toDouble();
        return std::isfinite(number) ? QJsonValue(number) : QJsonValue();
    }
    default:
        return QJsonValue::fromVariant(value);
    }
}

QString MAVLinkConsoleAIController::_trimConsoleContext(const QString &consoleText, bool *truncated) const
{
    if (truncated) {
        *truncated = false;
    }

    const QString trimmedText = consoleText.trimmed();
    if (trimmedText.length() <= kConsoleContextMaxChars) {
        return trimmedText;
    }

    if (truncated) {
        *truncated = true;
    }

    return trimmedText.right(kConsoleContextMaxChars);
}

void MAVLinkConsoleAIController::_appendConversationMessage(const QString &role, const QString &content)
{
    if (role.isEmpty() || content.trimmed().isEmpty()) {
        return;
    }

    _conversationHistory.append(QJsonObject{
        { QStringLiteral("role"), role },
        { QStringLiteral("content"), content.trimmed() }
    });

    while (_conversationHistory.size() > kConversationMessageLimit) {
        _conversationHistory.removeAt(0);
    }
}

void MAVLinkConsoleAIController::_setBusy(bool busy)
{
    if (_busy == busy) {
        return;
    }

    _busy = busy;
    emit busyChanged();
}

void MAVLinkConsoleAIController::_setErrorText(const QString &errorText)
{
    if (_errorText == errorText) {
        return;
    }

    _errorText = errorText;
    emit errorTextChanged();
}

void MAVLinkConsoleAIController::_failRequest(const QString &errorText)
{
    _setErrorText(errorText);
    if (!errorText.isEmpty()) {
        emit requestFailed(errorText);
    }
}

void MAVLinkConsoleAIController::_clearReply(bool abortReply)
{
    if (!_reply) {
        _timeoutTimer.stop();
        return;
    }

    QNetworkReply *reply = _reply;
    _reply = nullptr;
    (void) disconnect(reply, nullptr, this, nullptr);
    if (abortReply) {
        reply->abort();
    }
    reply->deleteLater();
    _timeoutTimer.stop();
}

void MAVLinkConsoleAIController::_setOAuthBusy(bool busy)
{
    if (_oauthBusy == busy) {
        return;
    }

    _oauthBusy = busy;
    emit oauthBusyChanged();
}

void MAVLinkConsoleAIController::_setOAuthStatusText(const QString &statusText)
{
    if (_oauthStatusText == statusText) {
        return;
    }

    _oauthStatusText = statusText;
    emit oauthStatusTextChanged();
}

void MAVLinkConsoleAIController::_failOAuth(const QString &errorText)
{
    _oauthPollTimer.stop();
    _clearOAuthReply(true);
    _setOAuthStatusText(errorText);
}

void MAVLinkConsoleAIController::_clearOAuthReply(bool abortReply)
{
    if (!_oauthReply) {
        return;
    }

    QNetworkReply *reply = _oauthReply;
    _oauthReply = nullptr;
    (void) disconnect(reply, nullptr, this, nullptr);
    if (abortReply) {
        reply->abort();
    }
    reply->deleteLater();
}

void MAVLinkConsoleAIController::_clearOAuthAuthorizationFields()
{
    _oauthPollTimer.stop();
    _oauthDeviceCode.clear();
    _oauthUserCode.clear();
    _oauthVerificationUri.clear();
    _oauthVerificationUriComplete.clear();
    _oauthMessage.clear();
    _oauthDeviceCodeExpiresAtUtc = QDateTime();
    _oauthPollIntervalMsec = 5000;
    emit oauthAuthorizationChanged();
}
