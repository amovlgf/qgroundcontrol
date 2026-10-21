#pragma once

#include <QtCore/QTimer>
#include <QtQmlIntegration/QtQmlIntegration>

#include "MAVLinkMessageType.h"
#include "UomSession.h"

class Vehicle;

class VehicleUomManager final : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("")
    Q_PROPERTY(bool supported READ supported NOTIFY changed)
    Q_PROPERTY(bool required READ required NOTIFY changed)
    Q_PROPERTY(bool simulation READ simulation NOTIFY changed)
    Q_PROPERTY(bool preflightReady READ preflightReady NOTIFY changed)
    Q_PROPERTY(bool activated READ activated NOTIFY changed)
    Q_PROPERTY(QString aircraftId READ aircraftId NOTIFY changed)
    Q_PROPERTY(QString registrationId READ registrationId NOTIFY changed)
    Q_PROPERTY(QString moduleId READ moduleId NOTIFY changed)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed)
    Q_PROPERTY(QString identityStatus READ identityStatus NOTIFY changed)
    Q_PROPERTY(quint32 activationError READ activationError NOTIFY changed)
    Q_PROPERTY(QString activationResultText READ activationResultText NOTIFY changed)
    Q_PROPERTY(quint32 identityRevision READ identityRevision NOTIFY changed)
    Q_PROPERTY(quint32 pendingRevision READ pendingRevision NOTIFY changed)
    Q_PROPERTY(quint32 lastAckAgeMs READ lastAckAgeMs NOTIFY changed)
public:
    explicit VehicleUomManager(Vehicle* vehicle);
    ~VehicleUomManager() override;
    void mavlinkMessageReceived(const mavlink_message_t& message);

    bool supported() const { return _supported; }

    bool required() const;

    bool simulation() const { return _session->environment() == Uom::Environment::Mock; }

    bool preflightReady() const { return _session->officialReady() || _session->simulationReady(); }

    bool activated() const { return _session->identity().activated; }

    QString aircraftId() const { return _session->identity().aircraftId; }

    QString registrationId() const { return _session->identity().registrationId; }

    QString moduleId() const { return _session->identity().moduleId; }

    QString statusText() const { return _session->detail(); }

    QString identityStatus() const { return _identityStatus; }

    quint32 activationError() const { return _activationError; }

    QString activationResultText() const;

    quint32 identityRevision() const { return _session->identity().revision; }

    quint32 pendingRevision() const { return _pendingRevision; }

    quint32 lastAckAgeMs() const { return _session->lastAckAgeMs(); }

signals:
    void changed();

private:
    friend class VehicleUomManagerTest;
    void _tick();
    void _resetConnection();
    void _requestIdentity();
    void _sendStatus();
    void _updateTelemetry();
    void _handleIdentity(const mavlink_message_t& message);
    Vehicle* _vehicle;
    UomProvider* _provider;
    UomSession* _session;
    QTimer _timer;
    Uom::Telemetry _telemetry;
    QString _identityStatus;
    quint64 _sessionId = 0;
    quint64 _bootId = 0;
    quint64 _lastIdentityUs = 0;
    quint64 _lastQueryUs = 0;
    quint64 _lastStatusUs = 0;
    quint64 _positionReceivedUs = 0;
    quint64 _gpsReceivedUs = 0;
    quint64 _gpsSampleUtc = 0;
    quint32 _positionSampleBootMs = 0;
    quint32 _requestId = 0;
    quint32 _pendingRequest = 0;
    quint32 _sequence = 0;
    quint32 _pendingRevision = 0;
    quint32 _activationError = 0;
    bool _lastReady = false;
    unsigned _queryAttempts = 0;
    bool _supported = false;
    bool _gpsFix = false;
};
