#include "VehicleUomManagerTest.h"

#include <cstring>
#include <memory>

#include <QtCore/QScopeGuard>
#include <QtCore/QTemporaryDir>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtTest/QSignalSpy>

#include "MAVLinkLib.h"
#include "MAVLinkProtocol.h"
#include "ParameterManager.h"
#include "QGCPalette.h"
#include "RemoteIDSettings.h"
#include "SettingsManager.h"
#include "Vehicle.h"
#include "VehicleUomManager.h"

namespace {
void advertiseUom(MockLink* link, Vehicle* vehicle)
{
    if (!link || !vehicle) {
        return;
    }
    mavlink_message_t message{};
    mavlink_param_union_t value{};
    value.param_int32 = 1;
    mavlink_get_channel_status(link->outgoingMavlinkChannel())->flags &= ~MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
    mavlink_msg_param_value_pack_chan(vehicle->id(), vehicle->defaultComponentId(), link->outgoingMavlinkChannel(),
                                      &message, "COM_RID_UOM", value.param_float, MAV_PARAM_TYPE_INT32, 1, UINT16_MAX);
    link->respondWithMavlinkMessage(message);
}

mavlink_amov_rid_identity_t responseFor(const mavlink_amov_rid_identity_request_t& request)
{
    Uom::Identity id;
    id.aircraftId = "TESTAIRCRAFT00000001";
    id.moduleId = "001122AABBCC";
    id.bootId = 41;
    id.ridBootId = 7;
    id.revision = 1;
    id.activated = true;
    id.digest = id.computedDigest();
    mavlink_amov_rid_identity_t response{};
    response.session_id = request.session_id;
    response.request_id = request.request_id;
    response.boot_id = id.bootId;
    response.rid_boot_id = id.ridBootId;
    response.identity_revision = id.revision;
    response.protocol_version = request.protocol_version;
    response.capabilities = request.protocol_version == 2 ? 3 : 0;
    response.result = AMOV_RID_IDENTITY_RESULT_OK;
    response.activation_state = AMOV_RID_ACTIVATION_STATE_ACTIVE;
    response.rid_node_id = 125;
    response.target_system = MAVLinkProtocol::instance()->getSystemId();
    response.target_component = MAVLinkProtocol::getComponentId();
    std::memcpy(response.uas_id, id.aircraftId.toLatin1().constData(), sizeof(response.uas_id));
    std::memcpy(response.rid_uid, id.moduleId.toLatin1().constData(), sizeof(response.rid_uid));
    std::memcpy(response.identity_hash, id.digest.constData(), sizeof(response.identity_hash));
    return response;
}
}  // namespace

void VehicleUomManagerTest::_identityBindingAndMavlink2()
{
    QVERIFY(vehicle());
    QVERIFY(mockLink());
    auto* manager = vehicle()->uomManager();
    QVERIFY(manager);
    // Exercise the first extended query after a link was still marked MAVLink 1.
    mavlink_get_channel_status(mockLink()->mavlinkChannel())->flags |= MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
    advertiseUom(mockLink(), vehicle());
    QTRY_VERIFY_WITH_TIMEOUT(manager->supported(), TestTimeout::mediumMs());
    mavlink_message_t query{};
    QTRY_VERIFY_WITH_TIMEOUT(mockLink()->lastReceivedMavlinkMessage(MAVLINK_MSG_ID_AMOV_RID_IDENTITY_REQUEST, query),
                             TestTimeout::mediumMs());
    QCOMPARE(query.magic, quint8(MAVLINK_STX));
    mavlink_amov_rid_identity_request_t request{};
    mavlink_msg_amov_rid_identity_request_decode(&query, &request);
    QVERIFY(request.session_id != 0);
    QVERIFY(request.request_id != 0);
    auto response = responseFor(request);
    mavlink_message_t message{};
    mavlink_msg_amov_rid_identity_encode(vehicle()->id(), vehicle()->defaultComponentId(), &message, &response);
    mockLink()->respondWithMavlinkMessage(message);
    QTRY_VERIFY_WITH_TIMEOUT(manager->activated(), TestTimeout::mediumMs());
    QCOMPARE(manager->aircraftId(), QString("TESTAIRCRAFT00000001"));
    QVERIFY(manager->registrationId().isEmpty());
    QCOMPARE(manager->identityRevision(), response.identity_revision);
    QVERIFY(!manager->preflightReady());
    mavlink_message_t heartbeat{};
    QTRY_VERIFY_WITH_TIMEOUT(mockLink()->lastReceivedMavlinkMessage(MAVLINK_MSG_ID_AMOV_UOM_STATUS, heartbeat),
                             TestTimeout::mediumMs());
    mavlink_amov_uom_status_t status{};
    mavlink_msg_amov_uom_status_decode(&heartbeat, &status);
    QCOMPARE(status.session_id, request.session_id);
    QCOMPARE(status.boot_id, response.boot_id);
    QCOMPARE(status.rid_boot_id, response.rid_boot_id);
    QCOMPARE(status.environment, quint8(AMOV_UOM_ENVIRONMENT_UNCONFIGURED));
    QCOMPARE(status.last_ack_age_ms, UINT32_MAX);

    manager->_pendingRequest = 0;
    manager->_requestIdentity();
    const auto stableGeneration = manager->_session->generation();
    response.request_id = manager->_pendingRequest;
    mavlink_msg_amov_rid_identity_encode(vehicle()->id(), vehicle()->defaultComponentId(), &message, &response);
    mockLink()->respondWithMavlinkMessage(message);
    QTRY_COMPARE_WITH_TIMEOUT(manager->_pendingRequest, quint32(0), TestTimeout::mediumMs());
    QCOMPARE(manager->_session->generation(), stableGeneration);
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&]() {
            if (!mockLink()->lastReceivedMavlinkMessage(MAVLINK_MSG_ID_AMOV_RID_IDENTITY_REQUEST, query)) {
                return false;
            }
            mavlink_msg_amov_rid_identity_request_decode(&query, &request);
            return request.request_id == response.request_id;
        })(),
        TestTimeout::mediumMs());
    QCOMPARE(request.flags, quint8(0));
    manager->_requestIdentity();
    const auto pending = manager->_pendingRequest;
    // An old response arriving after a new query must not replace the identity.
    response.uas_id[0] = 'X';
    QSignalSpy received(MAVLinkProtocol::instance(), &MAVLinkProtocol::messageReceived);
    mavlink_msg_amov_rid_identity_encode(vehicle()->id(), vehicle()->defaultComponentId(), &message, &response);
    mockLink()->respondWithMavlinkMessage(message);
    QVERIFY(received.wait(TestTimeout::mediumMs()));
    QCOMPARE(manager->aircraftId(), QString("TESTAIRCRAFT00000001"));
    // A matching response with a tampered digest invalidates the binding.
    response.request_id = pending;
    mavlink_msg_amov_rid_identity_encode(vehicle()->id(), vehicle()->defaultComponentId(), &message, &response);
    mockLink()->respondWithMavlinkMessage(message);
    QTRY_VERIFY_WITH_TIMEOUT(!manager->activated(), TestTimeout::mediumMs());
    QVERIFY(!manager->preflightReady());

    const auto oldSession = manager->_sessionId;
    // Deliberately losing the link can exhaust an outstanding peripheral discovery request.
    ignoreLogMessage("Vehicle.MavCommandQueue", QtWarningMsg,
                     QRegularExpression("Giving up sending command after max retries: MAV_CMD_REQUEST_MESSAGE message: "
                                        "GIMBAL_MANAGER_INFORMATION"));
    simulateCommLoss(true);
    QTRY_VERIFY_WITH_TIMEOUT(manager->_sessionId != oldSession, TestTimeout::mediumMs());
    QVERIFY(manager->aircraftId().isEmpty());
    simulateCommLoss(false);
}

void VehicleUomManagerTest::_identityTimeout()
{
    QVERIFY(vehicle());
    auto* manager = vehicle()->uomManager();
    QVERIFY(manager);
    advertiseUom(mockLink(), vehicle());
    QTRY_VERIFY_WITH_TIMEOUT(manager->_pendingRequest != 0, TestTimeout::mediumMs());
    const auto readingRequest = manager->_pendingRequest;
    manager->_lastQueryUs = manager->_session->nowUs() - 3000001;
    manager->_tick();
    QCOMPARE(manager->_pendingRequest, readingRequest);
    manager->_lastQueryUs = manager->_session->nowUs() - 8000001;
    manager->_tick();
    QCOMPARE(manager->_pendingRequest, readingRequest);
    manager->_lastQueryUs = manager->_session->nowUs() - 22000001;
    manager->_queryAttempts = 3;
    manager->_tick();
    QCOMPARE(manager->_pendingRequest, quint32(0));
    QVERIFY(!manager->preflightReady());
    QVERIFY(manager->identityStatus().contains("timed out"));
    manager->_lastQueryUs = manager->_session->nowUs() - 10000001;
    manager->_tick();
    QVERIFY(manager->_pendingRequest != 0);
    QVERIFY(manager->_pendingRequest != readingRequest);
}

void VehicleUomManagerTest::_oldTelemetryDoesNotRenewFreshness()
{
    QVERIFY(vehicle());
    auto* manager = vehicle()->uomManager();
    QVERIFY(manager);
    mavlink_message_t message{};
    mavlink_gps_raw_int_t gps{};
    gps.fix_type = 3;
    gps.time_usec = 1800000000000000ULL;
    mavlink_msg_gps_raw_int_encode(vehicle()->id(), vehicle()->defaultComponentId(), &message, &gps);
    manager->mavlinkMessageReceived(message);
    const auto gpsReceived = manager->_gpsReceivedUs;
    QVERIFY(gpsReceived != 0);
    --gps.time_usec;
    mavlink_msg_gps_raw_int_encode(vehicle()->id(), vehicle()->defaultComponentId(), &message, &gps);
    manager->mavlinkMessageReceived(message);
    QCOMPARE(manager->_gpsReceivedUs, gpsReceived);
    QCOMPARE(manager->_gpsSampleUtc, quint64(1800000000000000ULL));
    ++gps.time_usec;
    mavlink_msg_gps_raw_int_encode(vehicle()->id(), vehicle()->defaultComponentId(), &message, &gps);
    manager->mavlinkMessageReceived(message);
    QCOMPARE(manager->_gpsReceivedUs, gpsReceived);

    gps.time_usec += 60000000;
    mavlink_msg_gps_raw_int_encode(vehicle()->id(), vehicle()->defaultComponentId(), &message, &gps);
    manager->mavlinkMessageReceived(message);
    QCOMPARE(manager->_gpsSampleUtc, quint64(1800000000000000ULL));
    QVERIFY(!manager->_gpsFix);
    gps.time_usec = 1800000000000001ULL;
    mavlink_msg_gps_raw_int_encode(vehicle()->id(), vehicle()->defaultComponentId(), &message, &gps);
    manager->mavlinkMessageReceived(message);
    QVERIFY(manager->_gpsFix);

    gps.fix_type = 0;
    ++gps.time_usec;
    mavlink_msg_gps_raw_int_encode(vehicle()->id(), vehicle()->defaultComponentId(), &message, &gps);
    manager->mavlinkMessageReceived(message);
    QVERIFY(!manager->_gpsFix);
    gps.fix_type = 3;
    --gps.time_usec;
    mavlink_msg_gps_raw_int_encode(vehicle()->id(), vehicle()->defaultComponentId(), &message, &gps);
    manager->mavlinkMessageReceived(message);
    QVERIFY(!manager->_gpsFix);

    mavlink_global_position_int_t position{};
    position.time_boot_ms = 1000;
    position.lat = 470000000;
    manager->_positionReceivedUs = 0;
    mavlink_msg_global_position_int_encode(vehicle()->id(), vehicle()->defaultComponentId(), &message, &position);
    manager->mavlinkMessageReceived(message);
    const auto positionReceived = manager->_positionReceivedUs;
    position.time_boot_ms = 999;
    position.lat = 480000000;
    mavlink_msg_global_position_int_encode(vehicle()->id(), vehicle()->defaultComponentId(), &message, &position);
    manager->mavlinkMessageReceived(message);
    QCOMPARE(manager->_positionReceivedUs, positionReceived);
    QCOMPARE(manager->_telemetry.latitude, 47.0);
    position.time_boot_ms = 1000;
    mavlink_msg_global_position_int_encode(vehicle()->id(), vehicle()->defaultComponentId(), &message, &position);
    manager->mavlinkMessageReceived(message);
    QCOMPARE(manager->_telemetry.latitude, 47.0);

    manager->_positionSampleBootMs = UINT32_MAX - 5;
    position.time_boot_ms = 5;
    mavlink_msg_global_position_int_encode(vehicle()->id(), vehicle()->defaultComponentId(), &message, &position);
    manager->mavlinkMessageReceived(message);
    QCOMPARE(manager->_telemetry.latitude, 48.0);
    manager->_resetConnection();
    QCOMPARE(manager->_gpsSampleUtc, quint64(0));
    QCOMPARE(manager->_positionSampleBootMs, quint32(0));
}

void VehicleUomManagerTest::_statusPanelLoads()
{
    QVERIFY(vehicle());
    auto* manager = vehicle()->uomManager();
    advertiseUom(mockLink(), vehicle());
    QTRY_VERIFY_WITH_TIMEOUT(manager->supported(), TestTimeout::mediumMs());
    QQmlEngine engine;
    engine.addImportPath(QStringLiteral("qrc:/qml"));
    QGCPalette palette;
    engine.rootContext()->setContextProperty(QStringLiteral("qgcPal"), &palette);
    QSignalSpy warnings(&engine, &QQmlEngine::warnings);
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qml/QGroundControl/Toolbar/RemoteIDIndicatorPage.qml")));
    QTRY_VERIFY_WITH_TIMEOUT(!component.isLoading(), TestTimeout::mediumMs());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    const std::unique_ptr<QObject> page(
        component.createWithInitialProperties({{QStringLiteral("_activeVehicle"), QVariant::fromValue(vehicle())}}));
    QVERIFY2(page, qPrintable(component.errorString()));
    auto* panel = page->findChild<QObject*>(QStringLiteral("uomIdentityPanel"));
    QVERIFY(panel);
    QTRY_VERIFY_WITH_TIMEOUT(panel->property("width").toDouble() > 0, TestTimeout::mediumMs());
    auto* localStatus = page->findChild<QObject*>(QStringLiteral("uomToolbarLocalStatus"));
    auto* networkStatus = page->findChild<QObject*>(QStringLiteral("uomToolbarNetworkStatus"));
    QVERIFY(localStatus);
    QVERIFY(networkStatus);
    QCOMPARE(
        localStatus->property("text").toString(),
        QCoreApplication::translate("RemoteIDIndicatorPage", "Local readiness: %1").arg(manager->localStatusText()));
    QCOMPARE(
        networkStatus->property("text").toString(),
        QCoreApplication::translate("RemoteIDIndicatorPage", "Network status: %1").arg(manager->networkStatusText()));
    auto* result = page->findChild<QObject*>(QStringLiteral("uomActivationResult"));
    QVERIFY(result);
    manager->_activationError = 1;
    emit manager->changed();
    QTRY_VERIFY_WITH_TIMEOUT(result->property("text").toString().contains("power off and on"), TestTimeout::mediumMs());
    manager->_activationError = 2;
    emit manager->changed();
    QTRY_VERIFY_WITH_TIMEOUT(result->property("text").toString().contains("already active"), TestTimeout::mediumMs());
    QVERIFY(page->setProperty("_activeVehicle", QVariant::fromValue(static_cast<Vehicle*>(nullptr))));
    QTRY_VERIFY_WITH_TIMEOUT(result->property("text").toString().isEmpty(), TestTimeout::mediumMs());
    QVERIFY(warnings.isEmpty());
}

void VehicleUomManagerTest::_localReadinessDoesNotDependOnPlatform()
{
    auto* manager = vehicle()->uomManager();
    QVERIFY(manager);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto* service = new UomReportingService(directory.filePath("reports.sqlite"), manager);
    manager->_reporting = service;
    QTRY_VERIFY_WITH_TIMEOUT(service->ready(), TestTimeout::mediumMs());
    manager->_timer.stop();
    const auto restart = qScopeGuard([manager]() { manager->_timer.start(); });
    Uom::Identity identity;
    identity.aircraftId = "TESTAIRCRAFT00000001";
    identity.moduleId = "001122AABBCC";
    identity.bootId = 41;
    identity.ridBootId = 7;
    identity.revision = 1;
    identity.activated = true;
    identity.digest = identity.computedDigest();
    manager->_session->setIdentity(identity);
    manager->_bootId = 41;
    manager->_protocolVersion = 2;
    manager->_capabilities = 3;
    UomReportingService::Binding binding;
    binding.aircraftIds = {identity.aircraftId};
    binding.profile = {.mode = UomReporting::Mode::Isolated,
                       .endpoint = QUrl("http://127.0.0.1:9/ridData"),
                       .appId = "offline-app",
                       .uasModel = "P600-NX-DC10-5",
                       .uscCode = "TEST-USC",
                       .platform = "QGC-test",
                       .credentialReference = "uom-reporting/isolated/test"};
    service->_configureIsolated({binding}, {{binding.profile.credentialReference, "offline-key"}});
    auto* settings = SettingsManager::instance()->remoteIDSettings();
    settings->locationType()->setRawValue(2);
    settings->latitudeFixed()->setRawValue(0.0);
    settings->longitudeFixed()->setRawValue(0.0);
    settings->altitudeFixed()->setRawValue(0.0);
    settings->fixedPositionConfirmed()->setRawValue(true);
    const auto clearConfirmation =
        qScopeGuard([settings]() { settings->fixedPositionConfirmed()->setRawValue(false); });
    const quint64 now = manager->_session->nowUs();
    manager->_positionReceivedUs = manager->_gpsReceivedUs = manager->_flightReceivedUs = now;
    manager->_gpsSampleUtc = 1800000000000000ULL;
    manager->_flightSourceUs = 8000000;
    manager->_positionSampleBootMs = 8000;
    manager->_gpsFix = manager->_ellipsoidValid = true;
    manager->_groundSpeed = 0.0;
    manager->_trackDegrees.reset();  // Stationary GNSS need not invent a track.
    manager->_flightFlags = AMOV_UOM_FLIGHT_FLAGS_BROADCAST_READY | AMOV_UOM_FLIGHT_FLAGS_SIMULATION;
    manager->_updateTelemetry();
    QVERIFY(manager->localReady());
    QVERIFY(manager->preflightReady());
    QVERIFY(!manager->networkHealthy());
    QCOMPARE(manager->lastAckAgeMs(), UINT32_MAX);
    QVERIFY(manager->networkStatusText().startsWith(VehicleUomManager::tr("Isolated test: %1").arg(QString{})));
    manager->_supported = true;
    manager->_sendStatus();
    mavlink_message_t wire{};
    QTRY_VERIFY_WITH_TIMEOUT(mockLink()->lastReceivedMavlinkMessage(MAVLINK_MSG_ID_AMOV_UOM_STATUS, wire),
                             TestTimeout::mediumMs());
    mavlink_amov_uom_status_t status{};
    mavlink_msg_amov_uom_status_decode(&wire, &status);
    QCOMPARE(status.protocol_version, quint8(2));
    QCOMPARE(status.local_ready, quint8(1));
    QCOMPARE(status.local_error, quint16(AMOV_UOM_ERROR_NONE));
    QCOMPARE(status.flags & AMOV_UOM_FLAGS_AUTHENTICATED, 0);
    QCOMPARE(status.last_ack_age_ms, UINT32_MAX);
    service->_status[identity.aircraftId] = {{"networkFault", true}, {"rejected", true}, {"ackAgeMs", 10000}};
    QVERIFY(manager->localReady());
    QVERIFY(manager->preflightReady());
    QVERIFY(!manager->networkHealthy());
    manager->_flightFlags &= ~AMOV_UOM_FLIGHT_FLAGS_BROADCAST_READY;
    QVERIFY(!manager->localReady());
    manager->_flightFlags |= AMOV_UOM_FLIGHT_FLAGS_BROADCAST_READY;
    manager->_flightReceivedUs = now - 1000001;
    QVERIFY(!manager->localReady());
    manager->_flightReceivedUs = now;
    manager->_ellipsoidValid = false;
    QVERIFY(!manager->localReady());
    manager->_ellipsoidValid = true;
    manager->_positionSampleBootMs = 3000;
    QVERIFY(!manager->_reportSampleValid());  // Fresh arrival cannot make a five-second-old position current.
    manager->_flightSourceUs = (quint64(UINT32_MAX) + 51) * 1000;
    manager->_positionSampleBootMs = UINT32_MAX - 20;
    QVERIFY(manager->_reportSampleValid());  // The shared boot clock may wrap its 32-bit millisecond field.
    manager->_flightSourceUs = 8000000;
    manager->_positionSampleBootMs = 8000;
    settings->fixedPositionConfirmed()->setRawValue(false);
    QVERIFY(!manager->localReady());
    settings->fixedPositionConfirmed()->setRawValue(true);
    manager->_protocolVersion = 1;
    QVERIFY(!manager->preflightReady());
    manager->_protocolVersion = 2;
    service->_storageReady = false;
    QVERIFY(!manager->preflightReady());
    QVERIFY(manager->_reportSampleValid());
    service->_storageReady = true;
    service->_isolatedKeys.clear();
    QVERIFY(!manager->preflightReady());
    QVERIFY(manager->_reportSampleValid());
    manager->_ellipsoidValid = false;
    QVERIFY(!manager->_reportSampleValid());
    manager->_ellipsoidValid = true;
    service->_configureIsolated({binding}, {{binding.profile.credentialReference, "offline-key"}});
    QVERIFY(manager->localReady());
    QVERIFY(!manager->_realAircraft());
}

void VehicleUomManagerTest::_localReadinessRequiresEncodableSample_data()
{
    QTest::addColumn<QString>("field");
    QTest::addColumn<double>("value");
    QTest::addColumn<bool>("ready");
    QTest::newRow("zero-height") << QString("operatorAltitude") << 0.0 << true;
    QTest::newRow("minimum-height") << QString("operatorAltitude") << -999.5 << true;
    QTest::newRow("maximum-height") << QString("operatorAltitude") << 31767.5 << true;
    QTest::newRow("reserved-height") << QString("operatorAltitude") << -1000.0 << false;
    QTest::newRow("overflow-height") << QString("operatorAltitude") << 40000.0 << false;
    QTest::newRow("reserved-coordinate") << QString("operatorLongitude") << -0.0000001 << false;
    QTest::newRow("adjacent-coordinate") << QString("operatorLongitude") << -0.0000002 << true;
    QTest::newRow("aircraft-height") << QString("aircraftAltitude") << 40000.0 << false;
    QTest::newRow("invalid-track") << QString("track") << 360.0 << false;
    QTest::newRow("reserved-descent") << QString("velocityDown") << 63.5 << false;
}

void VehicleUomManagerTest::_localReadinessRequiresEncodableSample()
{
    QFETCH(QString, field);
    QFETCH(double, value);
    QFETCH(bool, ready);
    auto* manager = vehicle()->uomManager();
    QVERIFY(manager);
    manager->_timer.stop();
    const auto restart = qScopeGuard([manager]() { manager->_timer.start(); });
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto* service = new UomReportingService(directory.filePath("reports.sqlite"), manager);
    manager->_reporting = service;
    QTRY_VERIFY_WITH_TIMEOUT(service->ready(), TestTimeout::mediumMs());
    Uom::Identity identity;
    identity.aircraftId = "TESTAIRCRAFT00000001";
    identity.moduleId = "001122AABBCC";
    identity.bootId = 41;
    identity.ridBootId = 7;
    identity.revision = 1;
    identity.activated = true;
    identity.digest = identity.computedDigest();
    manager->_session->setIdentity(identity);
    manager->_protocolVersion = 2;
    manager->_capabilities = 3;
    UomReportingService::Binding binding;
    binding.aircraftIds = {identity.aircraftId};
    binding.profile = {.mode = UomReporting::Mode::Isolated,
                       .endpoint = QUrl("http://127.0.0.1:9/ridData"),
                       .appId = "offline-app",
                       .uasModel = "P600-NX-DC10-5",
                       .uscCode = "TEST-USC",
                       .platform = "QGC-test",
                       .credentialReference = "uom-reporting/isolated/test"};
    service->_configureIsolated({binding}, {{binding.profile.credentialReference, "offline-key"}});
    auto* settings = SettingsManager::instance()->remoteIDSettings();
    settings->locationType()->setRawValue(2);
    settings->latitudeFixed()->setRawValue(0.0);
    settings->longitudeFixed()->setRawValue(field == "operatorLongitude" ? value : 0.0);
    settings->altitudeFixed()->setRawValue(field == "operatorAltitude" ? value : 0.0);
    settings->fixedPositionConfirmed()->setRawValue(true);
    const auto clearPosition = qScopeGuard([settings]() {
        settings->longitudeFixed()->setRawValue(0.0);
        settings->altitudeFixed()->setRawValue(0.0);
        settings->fixedPositionConfirmed()->setRawValue(false);
    });
    const quint64 now = manager->_session->nowUs();
    manager->_positionReceivedUs = manager->_gpsReceivedUs = manager->_flightReceivedUs = now;
    manager->_gpsSampleUtc = 1800000000000000ULL;
    manager->_flightSourceUs = 8000000;
    manager->_positionSampleBootMs = 8000;
    manager->_gpsFix = manager->_ellipsoidValid = true;
    manager->_groundSpeed = 0.0;
    manager->_trackDegrees = field == "track" ? std::optional<double>(value) : std::nullopt;
    manager->_altitudeEllipsoid = field == "aircraftAltitude" ? value : 0.0;
    manager->_telemetry.velocityDown = field == "velocityDown" ? value : 0.0;
    manager->_flightFlags = AMOV_UOM_FLIGHT_FLAGS_BROADCAST_READY | AMOV_UOM_FLIGHT_FLAGS_SIMULATION;
    manager->_updateTelemetry();
    QVERIFY(manager->_session->telemetryValid());
    QCOMPARE(manager->localReady(), ready);
    QCOMPARE(manager->preflightReady(), ready);
    QCOMPARE(manager->localStatusText() == VehicleUomManager::tr("Local reporting is ready"), ready);
}

void VehicleUomManagerTest::_flightContextRejectsStaleAndForeignMessages()
{
    auto* manager = vehicle()->uomManager();
    QVERIFY(manager);
    manager->_protocolVersion = 2;
    manager->_capabilities = 3;
    manager->_bootId = 41;
    manager->_sessionId = 91;
    manager->_flightSourceUs = manager->_flightReceivedUs = manager->_takeoffUtcUs = 0;
    manager->_flightSequence = 0;
    mavlink_amov_uom_flight_status_t status{};
    status.protocol_version = 2;
    status.boot_id = 41;
    status.session_id = 91;
    status.time_boot_us = 1000000;
    status.flight_sequence = 1;
    status.takeoff_utc_us = 1800000000000000ULL;
    status.flags =
        AMOV_UOM_FLIGHT_FLAGS_ACTIVE | AMOV_UOM_FLIGHT_FLAGS_AIRBORNE | AMOV_UOM_FLIGHT_FLAGS_TAKEOFF_UTC_VALID;
    status.target_system = MAVLinkProtocol::instance()->getSystemId();
    status.target_component = MAVLinkProtocol::getComponentId();
    const auto deliver = [&]() {
        mavlink_message_t message{};
        mavlink_msg_amov_uom_flight_status_encode(vehicle()->id(), vehicle()->defaultComponentId(), &message, &status);
        manager->mavlinkMessageReceived(message);
    };
    deliver();
    QCOMPARE(manager->_flightSequence, quint32(1));
    const quint64 received = manager->_flightReceivedUs;
    --status.time_boot_us;
    deliver();
    QCOMPARE(manager->_flightReceivedUs, received);
    status.time_boot_us += 2;
    ++status.session_id;
    status.flight_sequence = 2;
    deliver();
    QCOMPARE(manager->_flightSequence, quint32(1));
    --status.session_id;
    status.flight_sequence = 1;
    ++status.takeoff_utc_us;
    deliver();
    QCOMPARE(manager->_takeoffUtcUs, quint64(1800000000000000ULL));
    status.flight_sequence = 2;
    deliver();
    QCOMPARE(manager->_flightSequence, quint32(2));
    QCOMPARE(manager->_takeoffUtcUs, quint64(1800000000000001ULL));
    manager->_resetConnection();
    QCOMPARE(manager->_flightReceivedUs, quint64(0));
    QVERIFY(!manager->protocolCompatible());
}

void VehicleUomManagerTest::_legacyFallbackClearsV2Readiness()
{
    auto* manager = vehicle()->uomManager();
    QVERIFY(manager);
    manager->_timer.stop();
    const auto restart = qScopeGuard([manager]() { manager->_timer.start(); });
    manager->_supported = true;
    manager->_requestedProtocol = 2;
    manager->_protocolVersion = 2;
    manager->_capabilities = 3;
    manager->_bootId = 41;
    manager->_flightSequence = 9;
    manager->_pendingRequest = 17;
    mavlink_amov_rid_identity_request_t request{};
    request.session_id = manager->_sessionId;
    request.request_id = 17;
    request.protocol_version = 1;
    auto response = responseFor(request);
    response.result = AMOV_RID_IDENTITY_RESULT_UNSUPPORTED;
    mavlink_message_t message{};
    mavlink_msg_amov_rid_identity_encode(vehicle()->id(), vehicle()->defaultComponentId(), &message, &response);
    manager->mavlinkMessageReceived(message);
    QCOMPARE(manager->_requestedProtocol, quint8(1));
    QVERIFY(manager->_sessionId != request.session_id);
    QVERIFY(!manager->protocolCompatible());
    QVERIFY(!manager->preflightReady());
    QCOMPARE(manager->_flightSequence, quint32(0));
    QVERIFY(manager->aircraftId().isEmpty());
    manager->_requestIdentity();
    request.session_id = manager->_sessionId;
    request.request_id = manager->_pendingRequest;
    response = responseFor(request);
    mavlink_msg_amov_rid_identity_encode(vehicle()->id(), vehicle()->defaultComponentId(), &message, &response);
    manager->mavlinkMessageReceived(message);
    QVERIFY(manager->activated());
    QVERIFY(!manager->protocolCompatible());
    manager->_requestIdentity();
    response.request_id = manager->_pendingRequest;
    response.protocol_version = 2;
    response.capabilities = 3;
    mavlink_msg_amov_rid_identity_encode(vehicle()->id(), vehicle()->defaultComponentId(), &message, &response);
    manager->mavlinkMessageReceived(message);
    QVERIFY(!manager->activated());
    QVERIFY(!manager->protocolCompatible());
}

UT_REGISTER_TEST(VehicleUomManagerTest, TestLabel::Integration, TestLabel::Vehicle)
