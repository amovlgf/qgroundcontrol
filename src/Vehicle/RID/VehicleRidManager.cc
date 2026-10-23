#include "VehicleRidManager.h"

#include <cstring>

#include <QtCore/QCryptographicHash>
#include <QtCore/QRandomGenerator>
#include <QtCore/QTimer>

#include "FirmwarePlugin.h"
#include "MultiVehicleManager.h"
#include "Vehicle.h"
#include "VehicleLinkManager.h"

using amov_device::Operation;
using amov_device::Result;

namespace {
struct PendingFactoryProvision
{
    amov_device::Request request;
    QByteArray digest;
    QString serial;
    int aircraftClass = 0;
};

// A Vehicle is destroyed on link loss; receipts must survive its manager to prevent a second write.
QHash<QByteArray, PendingFactoryProvision> pendingFactoryProvisions;

quint32 transactionId()
{
    const quint32 value = QRandomGenerator::global()->generate();
    return value ? value : 1;
}

QByteArray responseUid(const amov_device::Response& response)
{
    return QByteArray(reinterpret_cast<const char*>(response.uid), amov_device::UID_SIZE);
}

QString responseSerial(const amov_device::Response& response)
{
    return QString::fromLatin1(reinterpret_cast<const char*>(response.value), response.value_length);
}
}  // namespace

VehicleRidManager::VehicleRidManager(Vehicle* vehicle)
    : VehicleRidManager(vehicle, QUrl(QStringLiteral("https://uom.amovlab.cn/api/v1/device/check-binding")))
{}

VehicleRidManager::VehicleRidManager(Vehicle* vehicle, const QUrl& platformEndpoint)
    : QObject(vehicle)
    , _vehicle(vehicle)
    , _transport(vehicle, this)
    , _activation(platformEndpoint, this)
    , _parameters(this)
    , _status(tr("Read RID devices to begin"))
{
    connect(&_transport, &RidTransportClient::completed, this, &VehicleRidManager::_receive);
    connect(&_transport, &RidTransportClient::failed, this, &VehicleRidManager::_transportFailed);
    connect(&_parameters, &RidParameterManager::changed, this, [this]() {
        emit parametersChanged();
        emit changed();
    });
    connect(&_activation, &RidActivationSyncController::failed, this, [this](const QString& detail) {
        _platformState = detail;
        _fail(detail);
    });
    connect(&_activation, &RidActivationSyncController::targetReady, this,
            [this](quint64 generation, const QString& serial, bool active) {
                if (generation != _generation || _stage != Stage::Platform) {
                    return;
                }
                if (serial != _serial || !_writeAllowed()) {
                    _fail(tr("Aircraft changed or was armed; activation was not written"));
                    return;
                }
                _platformState = active ? tr("Activated") : tr("Not activated");
                _targetActive = active;
                _querySerial = serial;
                auto request = _request(Operation::ACTIVATE);
                request.value_length = 21;
                request.value[0] = active ? 1 : 0;
                std::memcpy(request.value + 1, serial.toLatin1().constData(), 20);
                _stage = Stage::ActivationWrite;
                _status = tr("Saving platform state to RID");
                _send(request, true);
            });
    connect(MultiVehicleManager::instance(), &MultiVehicleManager::activeVehicleChanged, this, [this](Vehicle* active) {
        if (active != _vehicle && busy()) {
            _fail(tr("Active aircraft changed; read RID before retrying"));
        }
        emit changed();
    });
    if (_vehicle) {
        connect(_vehicle->vehicleLinkManager(), &VehicleLinkManager::communicationLostChanged, this,
                [this](bool) { _resetConnection(); });
        connect(_vehicle->vehicleLinkManager(), &VehicleLinkManager::allLinksRemoved, this,
                [this](Vehicle*) { _resetConnection(); });
        connect(_vehicle, &Vehicle::underwayChanged, this, [this](bool underway) {
            if (underway && busy()) {
                _fail(tr("Aircraft is underway; RID operation stopped. Read the current state before retrying"));
            }
            emit changed();
        });
        connect(_vehicle, &Vehicle::armedChanged, this, [this](bool armed) {
            if (armed && busy()) {
                _fail(tr("Aircraft armed; RID operation stopped. Read the current state before retrying"));
            }
            emit changed();
        });
    }
}

bool VehicleRidManager::available() const
{
    return _vehicle && _vehicle->firmwarePlugin()->supportsRidManagement();
}

bool VehicleRidManager::_writeAllowed() const
{
    return _vehicle && MultiVehicleManager::instance()->activeVehicle() == _vehicle && !_vehicle->armed() &&
           !_vehicle->underway() && _supported && _boot && _uid.size() == 6 &&
           !_vehicle->vehicleLinkManager()->communicationLost();
}

bool VehicleRidManager::canWrite() const
{
    return !busy() && !pendingFactoryProvisions.contains(_uid) && _writeAllowed();
}

bool VehicleRidManager::canProvision() const
{
    return canWrite() && !locked() && _factoryProvisionAvailable && _serial.isEmpty() && _activationState == 0;
}

QString VehicleRidManager::factoryIdentityError(const QString& serial, int aircraftClass) const
{
    if (!RidActivationProvider::validSerial(serial)) {
        return tr("Product code must contain exactly 20 printable ASCII characters without spaces");
    }
    if (aircraftClass < 0 || aircraftClass > 4) {
        return tr("Aircraft class must be between 0 and 4");
    }
    return {};
}

QStringList VehicleRidManager::devices() const
{
    QStringList names;
    for (const auto& device : _devices) {
        names.append(
            tr("Node %1 — %2").arg(device.node).arg(QString::fromLatin1(responseUid(device.info).toHex().toUpper())));
    }
    return names;
}

void VehicleRidManager::mavlinkMessageReceived(const mavlink_message_t& message)
{
    _transport.receive(message);
}

void VehicleRidManager::_resetConnection()
{
    ++_generation;
    _transport.reset();
    _activation.cancel();
    _stage = Stage::Idle;
    _supported = false;
    _factoryProvisionSupported = _factoryProvisionAvailable = false;
    _recoveringMaintenance = false;
    _devices.clear();
    _probeFailures.clear();
    _selectedDevice = -1;
    _node = 0;
    _uid.clear();
    _serial.clear();
    _boot = _revision = 0;
    _activationState = 2;
    _platformState = tr("Not queried");
    _parameters.clear();
    _parameterValue.fill('\0');
    _parameterValue.clear();
    _pending = {};
    _mutation = {};
    _mutationDigest.clear();
    _verifyDefaults = false;
    _status = _provisionOutstanding
                  ? tr("Factory identity outcome is unknown. Reconnect and read RID devices to check the original "
                       "transaction.")
                  : tr("Connection changed; read RID devices again");
    emit changed();
}

void VehicleRidManager::_fail(const QString& message)
{
    const bool uncertainProvision = _provisionOutstanding &&
                                    (_stage == Stage::ProvisionWrite || _stage == Stage::ProvisionVerify ||
                                     _stage == Stage::ProvisionClassVerify || _stage == Stage::ProvisionRevisionVerify);
    ++_generation;
    _transport.reset();
    _activation.cancel();
    _stage = Stage::Idle;
    _recoveringMaintenance = false;
    _parameterValue.fill('\0');
    _parameterValue.clear();
    _pending = {};
    _mutation = {};
    _mutationDigest.clear();
    _verifyDefaults = false;
    _status = uncertainProvision
                  ? tr("Factory identity outcome is unknown: %1. Read RID devices to check the original transaction.")
                        .arg(message)
                  : message;
    emit changed();
}

void VehicleRidManager::_finish(const QString& message)
{
    _stage = Stage::Idle;
    _parameterValue.fill('\0');
    _parameterValue.clear();
    _pending = {};
    _mutation = {};
    _mutationDigest.clear();
    _verifyDefaults = false;
    _status = message;
    emit changed();
}

amov_device::Request VehicleRidManager::_request(Operation operation) const
{
    amov_device::Request request;
    request.operation = operation;
    request.transaction = transactionId();
    request.boot = _boot;
    request.revision = operation == Operation::ACTIVATE ? _revision : 0;
    if (_uid.size() == 6) {
        std::memcpy(request.uid, _uid.constData(), 6);
    }
    return request;
}

bool VehicleRidManager::_send(amov_device::Request request, bool mutation)
{
    uint8_t bytes[amov_device::REQUEST_SIZE]{};
    if (!amov_device::encode_request(request, bytes, sizeof(bytes))) {
        _fail(tr("Invalid RID request"));
        return false;
    }
    _pending = request;
    std::memset(_pending.value, 0, sizeof(_pending.value));
    if (mutation) {
        _mutation = _pending;
        _mutationDigest = QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char*>(bytes), sizeof(bytes)),
                                                   QCryptographicHash::Blake2b_256);
        _statusAttempts = 0;
        if (_stage == Stage::ProvisionWrite) {
            _provisionRequest = _mutation;
            _provisionDigest = _mutationDigest;
            _provisionOutstanding = true;
            pendingFactoryProvisions.insert(
                _uid, {_provisionRequest, _provisionDigest, _provisionSerial, _provisionClass});
        }
    }
    const bool sent = _transport.send(1, _node, QByteArray(reinterpret_cast<const char*>(bytes), sizeof(bytes)));
    std::memset(bytes, 0, sizeof(bytes));
    std::memset(request.value, 0, sizeof(request.value));
    if (!sent) {
        if (mutation && _stage == Stage::ProvisionWrite) {
            _clearProvisionReceipt();
        }
        _fail(tr("RID link is unavailable"));
        return false;
    }
    emit changed();
    return true;
}

void VehicleRidManager::refresh()
{
    if (busy() || !available()) {
        return;
    }
    ++_generation;
    _activation.cancel();
    _transport.reset();
    _supported = false;
    _devices.clear();
    _probeFailures.clear();
    _selectedDevice = -1;
    _parameters.clear();
    _stage = Stage::Listing;
    _status = tr("Finding RID devices");
    if (!_transport.send(0, 0, {})) {
        _fail(tr("Device management is unavailable on this link"));
    }
    emit changed();
}

bool VehicleRidManager::_validInfo(const amov_device::Response& response)
{
    return response.operation == Operation::INFO && response.result == Result::OK && response.boot &&
           responseUid(response) != QByteArray(6, '\0') && response.total > 0 && response.total <= 128 &&
           (response.value_length == 0 || RidActivationProvider::validSerial(responseSerial(response)));
}

void VehicleRidManager::_probeFailed(const QString& reason)
{
    _probeFailures.append(tr("CAN node %1: %2").arg(_node).arg(reason));
    _probeNext();
}

QString VehicleRidManager::_discoverySummary(const QString& message) const
{
    return _probeFailures.isEmpty() ? message : message + QChar('\n') + _probeFailures.join(QChar('\n'));
}

void VehicleRidManager::_probeNext()
{
    if (_probeIndex < _nodes.size()) {
        _stage = Stage::Probing;
        _node = _nodes[_probeIndex++];
        auto request = _request(Operation::INFO);
        request.boot = request.revision = 0;
        std::memset(request.uid, 0, sizeof(request.uid));
        _send(request);
        return;
    }
    if (_recoveringMaintenance) {
        for (int i = 0; i < _devices.size(); ++i) {
            if (responseUid(_devices[i].info) == _uid && _devices[i].info.boot != _previousBoot) {
                if (responseSerial(_devices[i].info) != _maintenanceSerial ||
                    _devices[i].info.activation_state != _maintenanceActivationState) {
                    _fail(tr("RID restarted but aircraft identity or activation changed; verify the device"));
                    return;
                }
                _node = _devices[i].node;
                _applyInfo(_devices[i].info);
                _selectedDevice = i;
                _stage = Stage::MaintenanceStatus;
                _checkMutationStatus();
                return;
            }
        }
        if (_maintenanceDeadline.elapsed() >= 20000) {
            _fail(tr("RID restart could not be confirmed; read devices before retrying"));
        } else {
            const auto generation = _generation;
            QTimer::singleShot(1000, this, [this, generation]() {
                if (generation == _generation && _recoveringMaintenance) {
                    _recoverMaintenance();
                }
            });
        }
        return;
    }
    int selected = -1;
    for (int i = 0; i < _devices.size(); ++i) {
        if (responseUid(_devices[i].info) == _uid) {
            selected = i;
        }
    }
    if (selected < 0 && _devices.size() == 1) {
        selected = 0;
    }
    if (selected >= 0) {
        _select(selected);
    } else {
        const QString summary = _nodes.isEmpty()
                                    ? tr("The flight controller did not discover any online CAN nodes. "
                                         "Check RID power and the CAN connection.")
                                    : _devices.isEmpty()
                                          ? tr("CAN nodes were found, but no RID management identity was confirmed.")
                                          : tr("Select the RID device to manage");
        _finish(_discoverySummary(summary));
    }
}

void VehicleRidManager::selectDevice(int index)
{
    if (!busy() && index >= 0 && index < _devices.size()) {
        ++_generation;
        _transport.reset();
        _activation.cancel();
        _select(index);
    }
}

void VehicleRidManager::_select(int index)
{
    _selectedDevice = index;
    _node = _devices[index].node;
    _applyInfo(_devices[index].info);
    if (_provisionOutstanding &&
        _uid == QByteArray(reinterpret_cast<const char*>(_provisionRequest.uid), amov_device::UID_SIZE)) {
        _mutation = _provisionRequest;
        _mutationDigest = _provisionDigest;
        _statusAttempts = 0;
        _stage = Stage::ProvisionWrite;
        _status = tr("Checking the original factory identity transaction");
        _checkMutationStatus();
        emit changed();
        return;
    }
    _completionText = _discoverySummary(tr("RID parameters read"));
    _enumerate();
}

void VehicleRidManager::_applyInfo(const amov_device::Response& response)
{
    if (_uid != responseUid(response)) {
        _platformState = tr("Not queried");
    }
    _uid = responseUid(response);
    const auto receipt = pendingFactoryProvisions.constFind(_uid);
    _provisionOutstanding = receipt != pendingFactoryProvisions.cend();
    if (_provisionOutstanding) {
        _provisionRequest = receipt->request;
        _provisionDigest = receipt->digest;
        _provisionSerial = receipt->serial;
        _provisionClass = receipt->aircraftClass;
    }
    _boot = response.boot;
    _revision = response.revision;
    _total = response.total;
    _serial = responseSerial(response);
    _activationState = response.activation_state;
    _factoryProvisionSupported = response.default_length == 1 &&
                                 (response.default_value[0] & amov_device::FACTORY_PROVISION_CAPABLE);
    _factoryProvisionAvailable = _factoryProvisionSupported &&
                                 (response.default_value[0] & amov_device::FACTORY_PROVISION_AVAILABLE);
    _supported = true;
    emit changed();
}

void VehicleRidManager::_enumerate()
{
    _parameters.clear();
    _metadataIndex = 0;
    _stage = Stage::Metadata;
    _status = tr("Reading RID parameters");
    _readMetadata();
}

void VehicleRidManager::_readMetadata()
{
    if (_metadataIndex >= _total) {
        if (_verifyDefaults && !_parameters.defaultsMatch()) {
            _verifyDefaults = false;
            _fail(tr("RID restarted but parameter defaults could not be confirmed"));
            return;
        }
        _verifyDefaults = false;
        _finish(_completionText);
        return;
    }
    auto request = _request(Operation::METADATA);
    request.index = _metadataIndex;
    _send(request);
}

void VehicleRidManager::syncActivation()
{
    if (!canWrite()) {
        return;
    }
    ++_generation;
    _platformState = tr("Querying");
    _stage = Stage::SyncInfo;
    _status = tr("Reading aircraft product code");
    _send(_request(Operation::INFO));
}

void VehicleRidManager::provisionFactoryIdentity(const QString& expectedUid, const QString& serial, int aircraftClass)
{
    if (!canProvision()) {
        return;
    }
    if (expectedUid != uid()) {
        _fail(tr("RID module changed; confirm the hardware UID again"));
        return;
    }
    const QString error = factoryIdentityError(serial, aircraftClass);
    if (!error.isEmpty()) {
        _fail(error);
        return;
    }
    ++_generation;
    _provisionSerial = serial;
    _provisionClass = aircraftClass;
    _stage = Stage::ProvisionInfo;
    _status = tr("Checking RID before binding its factory identity");
    _send(_request(Operation::INFO));
}

void VehicleRidManager::writeParameter(const QString& name, const QString& value)
{
    if (!canWrite() || locked()) {
        return;
    }
    auto* entry = _parameters.find(name);
    if (!entry) {
        return;
    }
    QByteArray encoded;
    const QString error = entry->encodeValue(value, encoded);
    if (!error.isEmpty()) {
        _fail(error);
        return;
    }
    ++_generation;
    _parameterName = name;
    _parameterValue = encoded;
    _stage = Stage::ParameterInfo;
    _status = tr("Checking RID before saving %1").arg(name);
    _send(_request(Operation::INFO));
}

void VehicleRidManager::_clearProvisionReceipt()
{
    const QByteArray uid(reinterpret_cast<const char*>(_provisionRequest.uid), amov_device::UID_SIZE);
    const auto receipt = pendingFactoryProvisions.constFind(uid);
    if (receipt != pendingFactoryProvisions.cend() && receipt->request.transaction == _provisionRequest.transaction) {
        pendingFactoryProvisions.remove(uid);
    }
    _provisionOutstanding = false;
    _provisionRequest = {};
    _provisionDigest.clear();
}

void VehicleRidManager::_checkMutationStatus()
{
    if (++_statusAttempts > 5) {
        _fail(tr("RID operation outcome is unknown; read the device before retrying"));
        return;
    }
    const auto generation = _generation;
    QTimer::singleShot(250, this, [this, generation]() {
        if (generation == _generation && busy()) {
            auto request = _request(Operation::STATUS);
            request.transaction = _mutation.transaction;
            request.boot = 0;
            request.value_length = static_cast<quint8>(_mutationDigest.size());
            std::memcpy(request.value, _mutationDigest.constData(), _mutationDigest.size());
            _send(request);
        }
    });
}

void VehicleRidManager::_mutationSaved(const amov_device::Response& response)
{
    if (response.result == Result::ACCEPTED || response.result == Result::BUSY) {
        if (_stage == Stage::MaintenanceWrite && response.result == Result::ACCEPTED) {
            _recoveringMaintenance = true;
            _previousBoot = _boot;
            _maintenanceDeadline.start();
            _recoverMaintenance();
        } else {
            _checkMutationStatus();
        }
        return;
    }
    if (response.operation != _mutation.operation || response.result != Result::SAVED ||
        !(response.flags & amov_device::DURABLE)) {
        if (_stage == Stage::ProvisionWrite && response.operation == _mutation.operation &&
            response.result != Result::SAVED && response.result != Result::UNKNOWN &&
            response.result != Result::STORAGE_ERROR) {
            _clearProvisionReceipt();
        }
        _fail(_resultText(response.result));
        return;
    }
    if (_stage == Stage::ProvisionWrite) {
        _stage = Stage::ProvisionVerify;
        _status = tr("Reading back the saved factory identity");
        _send(_request(Operation::INFO));
    } else if (_stage == Stage::MaintenanceWrite) {
        _recoveringMaintenance = true;
        _previousBoot = _boot;
        _maintenanceDeadline.start();
        _recoverMaintenance();
    } else if (_stage == Stage::MaintenanceStatus) {
        _recoveringMaintenance = false;
        _verifyDefaults = _mutation.operation == Operation::RESET;
        _completionText = tr("RID restart confirmed; parameters refreshed");
        _enumerate();
    } else if (_stage == Stage::ActivationWrite) {
        _stage = Stage::ActivationVerify;
        _status = tr("Checking the effective RID activation state");
        _send(_request(Operation::INFO));
    } else {
        _stage = Stage::ParameterVerify;
        auto request = _request(Operation::GET);
        std::memcpy(request.name, _parameterName.toLatin1().constData(), _parameterName.size());
        _send(request);
    }
}

void VehicleRidManager::_receive(quint8 operation, quint8, const QByteArray& payload)
{
    if (_stage == Stage::Listing) {
        if (operation != 0) {
            _fail(tr("Unexpected device-list response"));
            return;
        }
        _nodes.clear();
        _probeFailures.clear();
        for (const char raw : payload) {
            const auto node = static_cast<quint8>(raw);
            if (node == 0 || node > 127) {
                _fail(tr("The flight controller returned an invalid CAN node list"));
                return;
            }
            if (!_nodes.contains(node)) {
                _nodes.append(node);
            }
        }
        _probeIndex = 0;
        _probeNext();
        return;
    }
    amov_device::Response response;
    if (!amov_device::decode_response(reinterpret_cast<const uint8_t*>(payload.constData()), payload.size(),
                                      response) ||
        response.transaction != _pending.transaction ||
        (response.operation != _pending.operation &&
         !(_pending.operation == Operation::STATUS && response.operation == _mutation.operation))) {
        if (_stage == Stage::Probing) {
            const QString reason = payload.size() == amov_device::RESPONSE_SIZE &&
                                           static_cast<uint8_t>(payload[0]) != amov_device::VERSION
                                       ? tr("Unsupported RID management protocol version %1")
                                             .arg(static_cast<uint8_t>(payload[0]))
                                       : tr("Invalid or unrelated RID management response");
            _probeFailed(reason);
        } else {
            _fail(tr("Invalid or unrelated RID management response"));
        }
        return;
    }
    if (_stage == Stage::Probing) {
        if (_validInfo(response)) {
            _devices.append({_node, response});
            _probeNext();
        } else {
            _probeFailed(response.result == Result::OK ? tr("Invalid RID identity response")
                                                       : _resultText(response.result));
        }
        return;
    }
    if (responseUid(response) != _uid || (_pending.operation != Operation::STATUS && response.boot != _boot)) {
        _supported = false;
        _fail(tr("RID device or boot changed; read devices again"));
        return;
    }
    if (_stage == Stage::ActivationWrite || _stage == Stage::ParameterWrite || _stage == Stage::MaintenanceWrite ||
        _stage == Stage::MaintenanceStatus || _stage == Stage::ProvisionWrite) {
        _mutationSaved(response);
        return;
    }
    if (response.result != Result::OK) {
        _fail(_resultText(response.result));
        return;
    }
    if (_stage == Stage::Metadata) {
        if (response.index != _metadataIndex || response.total != _total || !_parameters.append(response)) {
            _fail(tr("Invalid RID parameter description"));
            return;
        }
        ++_metadataIndex;
        _readMetadata();
    } else if (_stage == Stage::SyncInfo || _stage == Stage::ParameterInfo || _stage == Stage::ActivationVerify ||
               _stage == Stage::ProvisionInfo || _stage == Stage::ProvisionVerify) {
        if (!_validInfo(response)) {
            _fail(tr("Invalid RID identity response"));
            return;
        }
        _applyInfo(response);
        if (_stage == Stage::ProvisionVerify) {
            if (_serial != _provisionSerial || _activationState != 0 || !_revision || _factoryProvisionAvailable ||
                (response.flags & amov_device::LEGACY_IDENTITY)) {
                _fail(tr("RID factory identity readback did not confirm the requested unactivated identity"));
            } else {
                _stage = Stage::ProvisionClassVerify;
                auto request = _request(Operation::GET);
                std::strcpy(request.name, "RID_CLASS");
                _send(request);
            }
        } else if (_stage == Stage::ActivationVerify) {
            if (_serial != _querySerial || _activationState != (_targetActive ? 1 : 0)) {
                _fail(tr("RID activation readback differs from the platform result"));
            } else {
                _finish(_targetActive ? tr("Synchronization complete — RID activated")
                                      : tr("Synchronization complete — RID not activated"));
            }
        } else if (!_writeAllowed()) {
            _fail(tr("Aircraft must remain disarmed during RID changes"));
        } else if (_stage == Stage::ProvisionInfo) {
            if (!_factoryProvisionAvailable || !_serial.isEmpty() || _activationState != 0 || locked()) {
                _fail(tr("RID is no longer available for first-time factory identity provisioning; "
                         "refresh the device"));
                return;
            }
            auto request = _request(Operation::PROVISION);
            request.value_length = 21;
            request.value[0] = static_cast<uint8_t>(_provisionClass);
            std::memcpy(request.value + 1, _provisionSerial.toLatin1().constData(), 20);
            _stage = Stage::ProvisionWrite;
            _status = tr("Saving factory identity to RID");
            _send(request, true);
        } else if (_stage == Stage::SyncInfo) {
            _stage = Stage::Platform;
            _status = tr("Querying platform activation state");
            emit changed();
            _activation.begin(_generation, _serial);
        } else {
            auto* entry = _parameters.find(_parameterName);
            if (!entry) {
                _fail(tr("RID parameter no longer exists"));
                return;
            }
            auto request = _request(Operation::SET);
            request.type = entry->type();
            std::memcpy(request.name, _parameterName.toLatin1().constData(), _parameterName.size());
            request.value_length = static_cast<quint8>(_parameterValue.size());
            std::memcpy(request.value, _parameterValue.constData(), _parameterValue.size());
            _stage = Stage::ParameterWrite;
            _status = tr("Saving %1 to RID").arg(_parameterName);
            _send(request, true);
            std::memset(request.value, 0, sizeof(request.value));
            if (entry->masked()) {
                _parameterValue.fill('\0');
                _parameterValue.clear();
            }
        }
    } else if (_stage == Stage::ProvisionClassVerify || _stage == Stage::ProvisionRevisionVerify) {
        const bool checkingClass = _stage == Stage::ProvisionClassVerify;
        const char* expectedName = checkingClass ? "RID_CLASS" : "RID_ID_REV";
        const uint32_t expectedValue = checkingClass ? uint32_t(_provisionClass) : 1U;
        if (std::strcmp(response.name, expectedName) || response.type != amov_device::Type::UINT32 ||
            response.value_length != 4 || amov_device::read_u32(response.value) != expectedValue ||
            response.activation_state != 0 || response.revision != _revision) {
            _fail(tr("RID factory identity fields did not confirm the saved class and identity version"));
            return;
        }
        if (checkingClass) {
            _stage = Stage::ProvisionRevisionVerify;
            auto request = _request(Operation::GET);
            std::strcpy(request.name, "RID_ID_REV");
            _send(request);
        } else {
            _clearProvisionReceipt();
            _platformState = tr("Not queried");
            _completionText = tr("Factory identity saved and verified. Synchronize platform activation next.");
            _enumerate();
        }
    } else if (_stage == Stage::ParameterVerify) {
        auto* entry = _parameters.find(_parameterName);
        const QByteArray actual(reinterpret_cast<const char*>(response.value), response.value_length);
        if (!entry || (!entry->masked() && actual != _parameterValue) || !entry->updateValue(response)) {
            _fail(tr("RID parameter readback did not confirm the saved value"));
            return;
        }
        _finish(entry->rebootRequired() ? tr("Saved %1; restart RID to apply").arg(_parameterName)
                                        : tr("Saved and verified %1").arg(_parameterName));
    }
}

void VehicleRidManager::_transportFailed(int status, const QString& message)
{
    if (_stage == Stage::Listing && !_recoveringMaintenance && status == 4) {
        _fail(tr("The flight controller did not answer the CAN-node discovery request. "
                 "Check the connection and matching flight-controller firmware."));
    } else if (_stage == Stage::Probing) {
        QString reason;
        switch (status) {
            case 1:
                reason = tr("Device-management transport is unsupported");
                break;
            case 2:
                reason = tr("The flight controller rejected the management request");
                break;
            case 3:
                reason = tr("Device-management transport is busy");
                break;
            case 4:
                reason = tr("RID INFO request timed out");
                break;
            case 5:
                reason = tr("CAN node is unavailable");
                break;
            default:
                reason = message;
                break;
        }
        _probeFailed(reason);
    } else if (_pending.operation == Operation::STATUS &&
               (_stage == Stage::ActivationWrite || _stage == Stage::ParameterWrite ||
                _stage == Stage::MaintenanceStatus || _stage == Stage::ProvisionWrite)) {
        if (status == 3 || status == 4 || status == 5) {
            _checkMutationStatus();
        } else {
            _fail(
                tr("RID operation outcome is unknown because its receipt could not be read; refresh before retrying"));
        }
    } else if (status == 4 && (_stage == Stage::ActivationWrite || _stage == Stage::ParameterWrite ||
                               _stage == Stage::MaintenanceStatus || _stage == Stage::ProvisionWrite)) {
        _checkMutationStatus();
    } else if (status == 4 && _stage == Stage::MaintenanceWrite) {
        _recoveringMaintenance = true;
        _previousBoot = _boot;
        _maintenanceDeadline.start();
        _recoverMaintenance();
    } else if (_recoveringMaintenance && _maintenanceDeadline.elapsed() < 20000) {
        _recoverMaintenance();
    } else {
        _fail(message);
    }
}

void VehicleRidManager::_startMaintenance(Operation operation)
{
    if (!canWrite() || (operation == Operation::RESET && locked())) {
        return;
    }
    ++_generation;
    _maintenanceSerial = _serial;
    _maintenanceActivationState = _activationState;
    _stage = Stage::MaintenanceWrite;
    _status = operation == Operation::RESET ? tr("Restoring RID defaults") : tr("Restarting RID");
    _send(_request(operation), true);
}

void VehicleRidManager::resetDefaults()
{
    _startMaintenance(Operation::RESET);
}

void VehicleRidManager::reboot()
{
    _startMaintenance(Operation::REBOOT);
}

void VehicleRidManager::_recoverMaintenance()
{
    _devices.clear();
    _stage = Stage::Listing;
    _status = tr("Waiting for RID to restart and reconnect");
    _transport.reset();
    if (!_transport.send(0, 0, {})) {
        _fail(tr("Link lost while waiting for RID restart; read devices again"));
    }
    emit changed();
}

QString VehicleRidManager::_resultText(Result result)
{
    switch (result) {
        case Result::LOCKED:
            return tr("RID parameters are locked");
        case Result::READ_ONLY:
            return tr("RID rejected a write to a read-only parameter");
        case Result::STORAGE_ERROR:
            return tr("RID could not save the change");
        case Result::CONFLICT:
            return tr("RID state changed; refresh before retrying");
        case Result::UNSUPPORTED:
            return tr("This RID firmware does not support the operation");
        case Result::UNKNOWN:
            return tr("RID operation outcome is unknown; refresh before retrying");
        default:
            return tr("RID rejected the operation (%1)").arg(static_cast<int>(result));
    }
}
