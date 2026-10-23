#pragma once

#include <QtCore/QElapsedTimer>
#include <QtCore/QVariantList>
#include <QtQmlIntegration/QtQmlIntegration>

#include "MAVLinkMessageType.h"
#include "RidActivationSyncController.h"
#include "RidParameterManager.h"
#include "RidTransportClient.h"

class Vehicle;

class VehicleRidManager final : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("RID management belongs to a vehicle")
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool supported READ supported NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool canWrite READ canWrite NOTIFY changed)
    Q_PROPERTY(bool locked READ locked NOTIFY changed)
    Q_PROPERTY(bool factoryProvisionSupported READ factoryProvisionSupported NOTIFY changed)
    Q_PROPERTY(bool canProvision READ canProvision NOTIFY changed)
    Q_PROPERTY(QString serial READ serial NOTIFY changed)
    Q_PROPERTY(QString uid READ uid NOTIFY changed)
    Q_PROPERTY(int nodeId READ nodeId NOTIFY changed)
    Q_PROPERTY(int activationState READ activationState NOTIFY changed)
    Q_PROPERTY(QString platformState READ platformState NOTIFY changed)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed)
    Q_PROPERTY(QVariantList parameters READ parameters NOTIFY parametersChanged)
    Q_PROPERTY(QStringList devices READ devices NOTIFY changed)
    Q_PROPERTY(int selectedDevice READ selectedDevice NOTIFY changed)
public:
    explicit VehicleRidManager(Vehicle* vehicle);
    void mavlinkMessageReceived(const mavlink_message_t& message);
    bool available() const;

    bool supported() const { return _supported; }

    bool busy() const { return _stage != Stage::Idle; }

    bool canWrite() const;

    bool locked() const { return _parameters.locked(); }

    bool factoryProvisionSupported() const { return _factoryProvisionSupported; }

    bool canProvision() const;

    QString serial() const { return _serial; }

    QString uid() const { return QString::fromLatin1(_uid.toHex().toUpper()); }

    int nodeId() const { return _node; }

    int activationState() const { return _activationState; }

    QString platformState() const { return _platformState; }

    QString statusText() const { return _status; }

    QVariantList parameters() const { return _parameters.entries(); }

    QStringList devices() const;

    int selectedDevice() const { return _selectedDevice; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void selectDevice(int index);
    Q_INVOKABLE void syncActivation();
    Q_INVOKABLE QString factoryIdentityError(const QString& serial, int aircraftClass) const;
    Q_INVOKABLE void provisionFactoryIdentity(const QString& expectedUid, const QString& serial, int aircraftClass);
    Q_INVOKABLE void writeParameter(const QString& name, const QString& value);
    Q_INVOKABLE void resetDefaults();
    Q_INVOKABLE void reboot();

signals:
    void changed();
    void parametersChanged();

private:
    friend class VehicleRidManagerTest;
    VehicleRidManager(Vehicle* vehicle, const QUrl& platformEndpoint);
    enum class Stage
    {
        Idle,
        Listing,
        Probing,
        Metadata,
        SyncInfo,
        Platform,
        ActivationWrite,
        ActivationVerify,
        ParameterInfo,
        ParameterWrite,
        ParameterVerify,
        ProvisionInfo,
        ProvisionWrite,
        ProvisionVerify,
        ProvisionClassVerify,
        ProvisionRevisionVerify,
        MaintenanceWrite,
        MaintenanceStatus
    };

    struct Device
    {
        quint8 node;
        amov_device::Response info;
    };

    void _resetConnection();
    void _fail(const QString& message);
    void _finish(const QString& message);
    bool _send(amov_device::Request request, bool mutation = false);
    amov_device::Request _request(amov_device::Operation operation) const;
    void _receive(quint8 operation, quint8 node, const QByteArray& payload);
    void _transportFailed(int status, const QString& message);
    void _probeNext();
    void _probeFailed(const QString& reason);
    QString _discoverySummary(const QString& message) const;
    void _select(int index);
    void _applyInfo(const amov_device::Response& response);
    void _enumerate();
    void _readMetadata();
    void _checkMutationStatus();
    void _clearProvisionReceipt();
    void _mutationSaved(const amov_device::Response& response);
    void _startMaintenance(amov_device::Operation operation);
    void _recoverMaintenance();
    bool _writeAllowed() const;
    static bool _validInfo(const amov_device::Response& response);
    static QString _resultText(amov_device::Result result);
    Vehicle* _vehicle;
    RidTransportClient _transport;
    RidActivationSyncController _activation;
    RidParameterManager _parameters;
    Stage _stage = Stage::Idle;
    amov_device::Request _pending;
    amov_device::Request _mutation;
    QByteArray _mutationDigest;
    QList<Device> _devices;
    QList<quint8> _nodes;
    QStringList _probeFailures;
    qsizetype _probeIndex = 0;
    int _selectedDevice = -1;
    quint8 _node = 0;
    QByteArray _uid;
    quint32 _boot = 0;
    quint32 _revision = 0;
    quint16 _total = 0;
    quint16 _metadataIndex = 0;
    quint64 _generation = 1;
    bool _supported = false;
    bool _factoryProvisionSupported = false;
    bool _factoryProvisionAvailable = false;
    bool _provisionOutstanding = false;
    QString _provisionSerial;
    int _provisionClass = 0;
    amov_device::Request _provisionRequest;
    QByteArray _provisionDigest;
    bool _targetActive = false;
    int _activationState = 2;
    QString _serial;
    QString _querySerial;
    QString _platformState = tr("Not queried");
    bool _verifyDefaults = false;
    QString _status;
    QString _parameterName;
    QByteArray _parameterValue;
    unsigned _statusAttempts = 0;
    bool _recoveringMaintenance = false;
    quint32 _previousBoot = 0;
    QString _maintenanceSerial;
    int _maintenanceActivationState = 2;
    QElapsedTimer _maintenanceDeadline;
    QString _completionText;
};
