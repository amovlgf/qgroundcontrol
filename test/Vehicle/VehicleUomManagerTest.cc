#include "VehicleUomManagerTest.h"

#include <cstring>
#include <memory>

#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtTest/QSignalSpy>

#include "MAVLinkLib.h"
#include "MAVLinkProtocol.h"
#include "ParameterManager.h"
#include "QGCPalette.h"
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
    response.protocol_version = 1;
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

UT_REGISTER_TEST(VehicleUomManagerTest, TestLabel::Integration, TestLabel::Vehicle)
