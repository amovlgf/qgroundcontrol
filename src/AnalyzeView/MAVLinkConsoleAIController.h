/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QObject>
#include <QtCore/QTimer>
#include <QtCore/QVariant>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtPositioning/QGeoCoordinate>

class Fact;
class FactGroup;
class Vehicle;

class MAVLinkConsoleAIController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool configured READ configured NOTIFY configuredChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged)

public:
    explicit MAVLinkConsoleAIController(QObject *parent = nullptr);
    ~MAVLinkConsoleAIController() override;

    bool busy() const { return _busy; }
    bool configured() const;
    QString errorText() const { return _errorText; }

    Q_INVOKABLE void ask(const QString &question);
    Q_INVOKABLE void cancel();

signals:
    void busyChanged();
    void configuredChanged();
    void errorTextChanged();
    void answerReady(const QString &answer);
    void requestFailed(const QString &errorText);

private slots:
    void _replyFinished();
    void _requestTimedOut();

private:
    QJsonObject _buildVehicleSnapshot(Vehicle *vehicle) const;
    QJsonObject _coordinateToJson(const QGeoCoordinate &coordinate) const;
    QJsonObject _factGroupToJson(const FactGroup *factGroup) const;
    QJsonObject _factToJson(const Fact *fact) const;
    QJsonValue _variantToJson(const QVariant &value) const;
    void _setBusy(bool busy);
    void _setErrorText(const QString &errorText);
    void _failRequest(const QString &errorText);
    void _clearReply(bool abortReply);

    QNetworkAccessManager _networkManager;
    QNetworkReply *_reply = nullptr;
    QTimer _timeoutTimer;
    QString _errorText;
    bool _busy = false;

    static constexpr int kRequestTimeoutMsec = 30000;
};
