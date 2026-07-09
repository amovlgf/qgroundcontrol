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
#include "MultiVehicleManager.h"
#include "QmlObjectListModel.h"
#include "SettingsManager.h"
#include "Vehicle.h"
#include "VehicleLinkManager.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QMetaType>
#include <QtCore/QVariant>
#include <QtNetwork/QNetworkRequest>
#include <QtPositioning/QGeoCoordinate>

#include <cmath>

MAVLinkConsoleAIController::MAVLinkConsoleAIController(QObject *parent)
    : QObject(parent)
    , _networkManager(this)
{
    _timeoutTimer.setSingleShot(true);
    _timeoutTimer.setInterval(kRequestTimeoutMsec);
    (void) connect(&_timeoutTimer, &QTimer::timeout, this, &MAVLinkConsoleAIController::_requestTimedOut);

    if (AIConsoleSettings *settings = SettingsManager::instance()->aiConsoleSettings()) {
        (void) connect(settings->endpointUrl(), &Fact::rawValueChanged, this, [this] { emit configuredChanged(); });
        (void) connect(settings->modelName(), &Fact::rawValueChanged, this, [this] { emit configuredChanged(); });
    }
}

MAVLinkConsoleAIController::~MAVLinkConsoleAIController()
{
    _clearReply(true);
}

bool MAVLinkConsoleAIController::configured() const
{
    AIConsoleSettings *settings = SettingsManager::instance()->aiConsoleSettings();
    return settings
        && !settings->endpointUrl()->rawValueString().trimmed().isEmpty()
        && !settings->modelName()->rawValueString().trimmed().isEmpty();
}

void MAVLinkConsoleAIController::ask(const QString &question)
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
    const QString apiKey = settings->apiKey()->rawValueString().trimmed();
    if (endpointUrl.isEmpty() || modelName.isEmpty()) {
        _failRequest(tr("Configure an AI endpoint URL and model first."));
        return;
    }

    Vehicle *vehicle = MultiVehicleManager::instance()->activeVehicle();
    if (!vehicle) {
        _failRequest(tr("No active vehicle is connected."));
        return;
    }

    const QUrl url = QUrl::fromUserInput(endpointUrl);
    if (!url.isValid()) {
        _failRequest(tr("The AI endpoint URL is invalid."));
        return;
    }

    _setErrorText(QString());

    const QJsonDocument snapshotDocument(_buildVehicleSnapshot(vehicle));
    const QString systemPrompt = tr(
        "You are a read-only QGroundControl flight status assistant. "
        "Answer in the user's language. Use only the provided JSON snapshot. "
        "Do not invent values. If a value is missing or telemetry is unavailable, say so. "
        "Never control, arm, disarm, change modes, set parameters, or execute shell/MAVLink commands. "
        "If asked to control the vehicle or run commands, refuse briefly and tell the user to use the MAVLink Console manually. "
        "Keep the answer concise.");

    const QString userContent = QStringLiteral("Question:\n%1\n\nCurrent QGroundControl vehicle status JSON:\n%2")
        .arg(trimmedQuestion, QString::fromUtf8(snapshotDocument.toJson(QJsonDocument::Compact)));

    QJsonArray messages;
    messages.append(QJsonObject{
        { QStringLiteral("role"), QStringLiteral("system") },
        { QStringLiteral("content"), systemPrompt }
    });
    messages.append(QJsonObject{
        { QStringLiteral("role"), QStringLiteral("user") },
        { QStringLiteral("content"), userContent }
    });

    const QJsonObject requestObject{
        { QStringLiteral("model"), modelName },
        { QStringLiteral("messages"), messages },
        { QStringLiteral("temperature"), 0.2 },
        { QStringLiteral("stream"), false }
    };

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader(QByteArrayLiteral("User-Agent"), QByteArrayLiteral("QGroundControl-MAVLinkConsoleAI"));
    if (!apiKey.isEmpty()) {
        request.setRawHeader(QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + apiKey.toUtf8());
    }

    _setBusy(true);
    _reply = _networkManager.post(request, QJsonDocument(requestObject).toJson(QJsonDocument::Compact));
    (void) connect(_reply, &QNetworkReply::finished, this, &MAVLinkConsoleAIController::_replyFinished);
    _timeoutTimer.start();
}

void MAVLinkConsoleAIController::cancel()
{
    if (!_reply) {
        return;
    }

    _clearReply(true);
    _setBusy(false);
    _failRequest(tr("AI request canceled."));
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
    const QString networkErrorText = reply->errorString();
    const QByteArray payload = reply->readAll();
    _clearReply(false);
    _setBusy(false);

    if (networkError != QNetworkReply::NoError) {
        _failRequest(networkErrorText);
        return;
    }

    QJsonParseError parseError{};
    const QJsonDocument responseDocument = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !responseDocument.isObject()) {
        _failRequest(tr("AI response was not valid JSON."));
        return;
    }

    const QJsonArray choices = responseDocument.object().value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        _failRequest(tr("AI response did not contain any choices."));
        return;
    }

    const QString answer = choices.at(0).toObject()
        .value(QStringLiteral("message")).toObject()
        .value(QStringLiteral("content")).toString().trimmed();
    if (answer.isEmpty()) {
        _failRequest(tr("AI response did not contain an answer."));
        return;
    }

    emit answerReady(answer);
}

void MAVLinkConsoleAIController::_requestTimedOut()
{
    if (!_reply) {
        return;
    }

    _clearReply(true);
    _setBusy(false);
    _failRequest(tr("AI request timed out."));
}

QJsonObject MAVLinkConsoleAIController::_buildVehicleSnapshot(Vehicle *vehicle) const
{
    QJsonObject snapshot;
    snapshot.insert(QStringLiteral("source"), QStringLiteral("QGroundControl active vehicle telemetry"));
    snapshot.insert(QStringLiteral("timestampUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));

    QJsonObject core;
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
    object.insert(QStringLiteral("value"), _variantToJson(fact->cookedValue()));
    object.insert(QStringLiteral("rawValue"), _variantToJson(fact->rawValue()));
    object.insert(QStringLiteral("valueString"), fact->cookedValueString());
    object.insert(QStringLiteral("units"), fact->cookedUnits());
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
