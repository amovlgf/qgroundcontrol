#include "VehicleUomManager.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include <QtCore/QDateTime>
#include <QtCore/QRandomGenerator>

#include "Fact.h"
#include "FirmwarePlugin.h"
#include "GPSManager.h"
#include "MAVLinkLib.h"
#include "MAVLinkProtocol.h"
#include "MockUomProvider.h"
#include "ParameterManager.h"
#include "PositionManager.h"
#include "QGCLoggingCategory.h"
#include "RemoteIDSettings.h"
#include "SettingsManager.h"
#include "Vehicle.h"
#include "VehicleLinkManager.h"

QGC_LOGGING_CATEGORY(VehicleUomLog, "Vehicle.UOM")

namespace {
QString boundedString(const char* bytes, qsizetype size)
{
    const QByteArray value(bytes, size);
    const auto end = value.indexOf('\0');
    if (end >= 0 && value.mid(end) != QByteArray(size - end, '\0')) {
        return QString(QChar(0));
    }
    return QString::fromLatin1(end < 0 ? value : value.left(end));
}

quint16 wireError(Uom::Fault fault)
{
    switch (fault) {
        case Uom::Fault::None:
            return AMOV_UOM_ERROR_NONE;
        case Uom::Fault::Identity:
            return AMOV_UOM_ERROR_IDENTITY;
        case Uom::Fault::Activation:
            return AMOV_UOM_ERROR_ACTIVATION;
        case Uom::Fault::Telemetry:
            return AMOV_UOM_ERROR_INVALID_DATA;
        case Uom::Fault::Authentication:
            return AMOV_UOM_ERROR_AUTHENTICATION;
        case Uom::Fault::Network:
            return AMOV_UOM_ERROR_NETWORK;
        case Uom::Fault::Response:
            return AMOV_UOM_ERROR_REJECTED;
        case Uom::Fault::Timeout:
            return AMOV_UOM_ERROR_TIMEOUT;
    }
    return AMOV_UOM_ERROR_PROTOCOL;
}

quint8 wireState(Uom::State state, bool acknowledged)
{
    switch (state) {
        case Uom::State::NotConfigured:
            return AMOV_UOM_STATE_NOT_CONFIGURED;
        case Uom::State::WaitingIdentity:
            return AMOV_UOM_STATE_WAIT_IDENTITY;
        case Uom::State::WaitingData:
            return AMOV_UOM_STATE_DEGRADED;
        case Uom::State::Authenticating:
            return AMOV_UOM_STATE_AUTHENTICATING;
        case Uom::State::Reporting:
            return acknowledged ? AMOV_UOM_STATE_REPORTING : AMOV_UOM_STATE_AUTHENTICATING;
        case Uom::State::Recovering:
            return AMOV_UOM_STATE_DEGRADED;
        case Uom::State::Fault:
            return AMOV_UOM_STATE_FAILED;
    }
    return AMOV_UOM_STATE_FAILED;
}
}  // namespace

VehicleUomManager::VehicleUomManager(Vehicle* vehicle)
    : QObject(vehicle)
    , _vehicle(vehicle)
    , _provider(new UnconfiguredUomProvider(this))
    , _session(nullptr)
{
#ifdef QGC_UOM_BENCH_TEST
    const QUrl endpoint(qEnvironmentVariable("QGC_UOM_MOCK_URL"));
    if (MockUomProvider::isLocalEndpoint(endpoint)) {
        delete _provider;
        _provider = new MockUomProvider(endpoint, this);
    }
#endif
    _session = new UomSession(_provider, this);
    connect(_session, &UomSession::changed, this, [this]() {
        if (_lastReady && !preflightReady() && _vehicle && _vehicle->airborne()) {
            qCWarning(VehicleUomLog) << "UOM reporting lost in flight; warning only, fault"
                                     << static_cast<int>(_session->fault());
        }
        _lastReady = preflightReady();
        qCDebug(VehicleUomLog) << "UOM state" << static_cast<int>(_session->state()) << "fault"
                               << static_cast<int>(_session->fault()) << "simulation" << simulation();
        emit changed();
        _sendStatus();
    });
    if (!_vehicle) {
        return;
    }
    connect(_vehicle->vehicleLinkManager(), &VehicleLinkManager::communicationLostChanged, this,
            [this](bool) { _resetConnection(); });
    connect(_vehicle->vehicleLinkManager(), &VehicleLinkManager::allLinksRemoved, this,
            [this](Vehicle*) { _resetConnection(); });
    _resetConnection();
    _timer.setInterval(200);
    connect(&_timer, &QTimer::timeout, this, &VehicleUomManager::_tick);
    _timer.start();
}

VehicleUomManager::~VehicleUomManager()
{
    delete _session;
}

bool VehicleUomManager::required() const
{
    return _supported && _vehicle &&
           _vehicle->parameterManager()->parameterExists(ParameterManager::defaultComponentId,
                                                         QStringLiteral("COM_RID_UOM")) &&
           _vehicle->parameterManager()
               ->getParameter(ParameterManager::defaultComponentId, QStringLiteral("COM_RID_UOM"))
               ->rawValue()
               .toBool();
}

QString VehicleUomManager::activationResultText() const
{
    switch (_activationError) {
        case 0:
            return tr("No activation error");
        case 1:
            return tr("Activation staged; manually power off and on to apply");
        case 2:
            return tr("This authorization is already active");
        case 3:
            return tr("Invalid activation package format");
        case 4:
            return tr("Activation signing key is not trusted");
        case 5:
            return tr("Activation signature verification failed");
        case 6:
            return tr("Activation package belongs to another device");
        case 7:
            return tr("Activation identity is invalid");
        case 8:
            return tr("Activation revision is older than the current revision");
        case 9:
            return tr("Activation revision conflicts with another package");
        case 10:
            return tr("Activation could not be saved to RID storage");
        case 11:
            return tr("RID is busy; retry activation later");
        default:
            return tr("Unknown activation result: %1").arg(_activationError);
    }
}

void VehicleUomManager::_resetConnection()
{
    _sessionId = QRandomGenerator::global()->generate64();
    if (!_sessionId) {
        _sessionId = 1;
    }
    _bootId = _lastIdentityUs = _lastQueryUs = _lastStatusUs = 0;
    _positionReceivedUs = _gpsReceivedUs = _gpsSampleUtc = 0;
    _pendingRequest = _pendingRevision = _activationError = _positionSampleBootMs = 0;
    _queryAttempts = 0;
    _gpsFix = false;
    _telemetry = {};
    _identityStatus = tr("Waiting for aircraft identity");
    _session->reset();
    emit changed();
}

void VehicleUomManager::_tick()
{
    if (!_vehicle || !_vehicle->firmwarePlugin()->supportsRidUom()) {
        return;
    }
    const auto* parameters = _vehicle->parameterManager();
    const bool supported =
        parameters->parametersReady() &&
        parameters->parameterExists(ParameterManager::defaultComponentId, QStringLiteral("COM_RID_UOM"));
    if (_supported != supported) {
        _supported = supported;
        emit changed();
    }
    if (!_supported || _vehicle->vehicleLinkManager()->communicationLost()) {
        return;
    }
    _updateTelemetry();
    const quint64 now = _session->nowUs();
    if (_lastIdentityUs && now - _lastIdentityUs > 10000000) {
        _lastIdentityUs = 0;
        _session->setIdentity({});
        _identityStatus = tr("Aircraft identity expired; reading again");
        emit changed();
    }
    if (_pendingRequest && now - _lastQueryUs >= 22000000) {
        _pendingRequest = 0;
        _lastQueryUs = now;
        _identityStatus = tr("Identity read timed out");
        emit changed();
    }
    const quint64 retryInterval = _queryAttempts < 3 ? 1000000 : 10000000;
    if (!_pendingRequest && (!_lastQueryUs || now - _lastQueryUs >= (_lastIdentityUs ? 5000000 : retryInterval))) {
        _requestIdentity();
    }
    if (!_lastStatusUs || now - _lastStatusUs >= 1000000) {
        _sendStatus();
        emit changed();
    }
}

void VehicleUomManager::_requestIdentity()
{
    if (!_vehicle) {
        return;
    }
    const auto link = _vehicle->vehicleLinkManager()->primaryLink().lock();
    if (!link || !link->isConnected() || link->isLogReplay()) {
        return;
    }
    // Extended message IDs cannot be encoded in MAVLink 1, including the first query on a fresh link.
    mavlink_get_channel_status(link->mavlinkChannel())->flags &= ~MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
    mavlink_amov_rid_identity_request_t request{};
    request.session_id = _sessionId;
    request.request_id = ++_requestId;
    if (!request.request_id) {
        request.request_id = ++_requestId;
    }
    request.target_system = _vehicle->id();
    request.target_component = _vehicle->defaultComponentId();
    request.protocol_version = 1;
    request.flags = _lastIdentityUs ? 0 : 1;
    mavlink_message_t message{};
    mavlink_msg_amov_rid_identity_request_encode_chan(MAVLinkProtocol::instance()->getSystemId(),
                                                      MAVLinkProtocol::getComponentId(), link->mavlinkChannel(),
                                                      &message, &request);
    _pendingRequest = request.request_id;
    _lastQueryUs = _session->nowUs();
    ++_queryAttempts;
    _vehicle->sendMessageOnLinkThreadSafe(link.get(), message);
}

void VehicleUomManager::mavlinkMessageReceived(const mavlink_message_t& message)
{
    if (!_vehicle || message.sysid != _vehicle->id() || message.compid != _vehicle->defaultComponentId()) {
        return;
    }
    switch (message.msgid) {
        case MAVLINK_MSG_ID_AMOV_RID_IDENTITY:
            _handleIdentity(message);
            break;
        case MAVLINK_MSG_ID_GPS_RAW_INT: {
            mavlink_gps_raw_int_t gps{};
            mavlink_msg_gps_raw_int_decode(&message, &gps);
            if (gps.fix_type < 3) {
                _gpsFix = false;
            }
            if (gps.time_usec >= 1577836800000000ULL && gps.time_usec > _gpsSampleUtc) {
                _gpsFix = gps.fix_type >= 3;
                _gpsSampleUtc = gps.time_usec;
                _gpsReceivedUs = _session->nowUs();
            }
            break;
        }
        case MAVLINK_MSG_ID_GLOBAL_POSITION_INT: {
            mavlink_global_position_int_t position{};
            mavlink_msg_global_position_int_decode(&message, &position);
            if (!_positionReceivedUs || static_cast<qint32>(position.time_boot_ms - _positionSampleBootMs) > 0) {
                _positionSampleBootMs = position.time_boot_ms;
                _positionReceivedUs = _session->nowUs();
                _telemetry.latitude = position.lat / 1e7;
                _telemetry.longitude = position.lon / 1e7;
                _telemetry.altitudeMsl = position.alt / 1000.0;
                _telemetry.relativeAltitude = position.relative_alt / 1000.0;
                _telemetry.velocityNorth = position.vx / 100.0;
                _telemetry.velocityEast = position.vy / 100.0;
                _telemetry.velocityDown = position.vz / 100.0;
            }
            break;
        }
        default:
            break;
    }
}

void VehicleUomManager::_handleIdentity(const mavlink_message_t& message)
{
    mavlink_amov_rid_identity_t response{};
    mavlink_msg_amov_rid_identity_decode(&message, &response);
    if (!_supported || response.target_system != MAVLinkProtocol::instance()->getSystemId() ||
        response.target_component != MAVLinkProtocol::getComponentId() || response.session_id != _sessionId) {
        return;
    }
    if (!response.request_id) {
        _resetConnection();
        return;
    }
    if (!_pendingRequest || response.request_id != _pendingRequest) {
        return;
    }
    if (response.result == AMOV_RID_IDENTITY_RESULT_READING && response.protocol_version == 1) {
        return;
    }
    _pendingRequest = 0;
    _pendingRevision = response.pending_revision;
    _activationError = response.activation_error;
    if (response.protocol_version != 1 || response.result != AMOV_RID_IDENTITY_RESULT_OK || response.boot_id == 0 ||
        response.rid_boot_id == 0 || response.rid_node_id == 0 || response.rid_node_id > 127) {
        _session->setIdentity({});
        _identityStatus =
            tr("Identity rejected (result %1, protocol %2)").arg(response.result).arg(response.protocol_version);
        emit changed();
        return;
    }
    if (_bootId && _bootId != response.boot_id) {
        _positionReceivedUs = _gpsReceivedUs = _gpsSampleUtc = 0;
        _positionSampleBootMs = 0;
        _gpsFix = false;
        _telemetry = {};
        _session->setTelemetry({});
    }
    _bootId = response.boot_id;
    Uom::Identity identity;
    identity.bootId = response.boot_id;
    identity.ridBootId = response.rid_boot_id;
    identity.revision = response.identity_revision;
    identity.aircraftClass = response.aircraft_class;
    identity.aircraftId = boundedString(response.uas_id, sizeof(response.uas_id));
    identity.registrationId = boundedString(response.registration_id, sizeof(response.registration_id));
    identity.broadcastSuffix = boundedString(response.uin, sizeof(response.uin));
    identity.moduleId = boundedString(response.rid_uid, sizeof(response.rid_uid));
    identity.digest = QByteArray(reinterpret_cast<const char*>(response.identity_hash), sizeof(response.identity_hash));
    identity.activated = response.activation_state == AMOV_RID_ACTIVATION_STATE_ACTIVE;
    if (identity.activated && !identity.valid()) {
        _session->setIdentity({});
        _identityStatus = tr("Aircraft identity fields or digest are invalid");
    } else {
        _session->setIdentity(identity);
        _lastIdentityUs = _session->nowUs();
        _queryAttempts = 0;
        _identityStatus = identity.activated ? tr("RID identity verified and bound to this aircraft")
                                             : tr("RID activation required through its Wi-Fi page");
    }
    emit changed();
}

void VehicleUomManager::_updateTelemetry()
{
    if (!_vehicle) {
        return;
    }
    _telemetry.receivedUs = std::min(_positionReceivedUs, _gpsReceivedUs);
    _telemetry.utcMilliseconds = _gpsSampleUtc / 1000;
    _telemetry.positionValid = _gpsFix;
    _telemetry.armed = _vehicle->armed();
    _telemetry.operatorValid = false;
    auto* settings = SettingsManager::instance()->remoteIDSettings();
    if (settings->locationType()->rawValue().toInt() == 2) {
        _telemetry.operatorLatitude = settings->latitudeFixed()->rawValue().toDouble();
        _telemetry.operatorLongitude = settings->longitudeFixed()->rawValue().toDouble();
        _telemetry.operatorAltitudeEllipsoid = settings->altitudeFixed()->rawValue().toDouble();
        _telemetry.operatorValid = true;
    } else {
        const auto observation = GPSManager::instance()->positionManager()->acceptedObservation(
            GPSObservation::PositionUse::RemoteID, std::chrono::milliseconds(1000));
        if (observation && observation->altitudeDatum == GPSAltitudeDatum::Ellipsoid) {
            const auto coordinate = observation->position.coordinate();
            _telemetry.operatorLatitude = coordinate.latitude();
            _telemetry.operatorLongitude = coordinate.longitude();
            _telemetry.operatorAltitudeEllipsoid = coordinate.altitude();
            _telemetry.operatorValid = true;
        }
    }
    _session->setTelemetry(_telemetry);
}

void VehicleUomManager::_sendStatus()
{
    if (!_vehicle || !_supported || !_bootId || _vehicle->vehicleLinkManager()->communicationLost()) {
        return;
    }
    const auto link = _vehicle->vehicleLinkManager()->primaryLink().lock();
    if (!link || !link->isConnected() || link->isLogReplay()) {
        return;
    }
    const auto& identity = _session->identity();
    mavlink_amov_uom_status_t status{};
    status.boot_id = _bootId;
    status.session_id = _sessionId;
    status.sequence = ++_sequence;
    status.identity_revision = identity.revision;
    status.rid_boot_id = identity.ridBootId;
    status.last_ack_age_ms = _session->lastAckAgeMs();
    status.target_system = _vehicle->id();
    status.target_component = _vehicle->defaultComponentId();
    status.protocol_version = 1;
    status.environment =
        simulation() ? AMOV_UOM_ENVIRONMENT_MOCK
                     : (_session->environment() == Uom::Environment::Official ? AMOV_UOM_ENVIRONMENT_OFFICIAL
                                                                              : AMOV_UOM_ENVIRONMENT_UNCONFIGURED);
    status.state = wireState(_session->state(), _session->acknowledged());
    status.error_code = _session->state() == Uom::State::NotConfigured ? quint16(AMOV_UOM_ERROR_NOT_CONFIGURED)
                                                                       : wireError(_session->fault());
    if (identity.valid()) {
        status.flags |= AMOV_UOM_FLAGS_IDENTITY_VALID;
    }
    if (identity.activated) {
        status.flags |= AMOV_UOM_FLAGS_ACTIVATION_VALID;
    }
    if (_session->telemetryValid()) {
        status.flags |= AMOV_UOM_FLAGS_TELEMETRY_VALID;
    }
    if (_session->authenticated()) {
        status.flags |= AMOV_UOM_FLAGS_AUTHENTICATED;
    }
    if (identity.digest.size() == static_cast<qsizetype>(sizeof(status.identity_hash))) {
        std::memcpy(status.identity_hash, identity.digest.constData(), sizeof(status.identity_hash));
    }
    mavlink_get_channel_status(link->mavlinkChannel())->flags &= ~MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
    mavlink_message_t message{};
    mavlink_msg_amov_uom_status_encode_chan(MAVLinkProtocol::instance()->getSystemId(),
                                            MAVLinkProtocol::getComponentId(), link->mavlinkChannel(), &message,
                                            &status);
    _lastStatusUs = _session->nowUs();
    _vehicle->sendMessageOnLinkThreadSafe(link.get(), message);
}
