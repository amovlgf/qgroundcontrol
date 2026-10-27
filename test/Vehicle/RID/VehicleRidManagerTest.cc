#include "VehicleRidManagerTest.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>

#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QScopeGuard>
#include <QtCore/QTemporaryFile>
#include <QtCore/QTimer>

#include "MAVLinkLib.h"
#include "MAVLinkProtocol.h"
#include "Vehicle.h"
#include "VehicleRidManager.h"

UT_REGISTER_TEST(VehicleRidManagerTest, TestLabel::Integration)

namespace {
using amov_device::Operation;
using amov_device::Result;
using amov_device::Type;
const QByteArray serial("TESTAIRCRAFT00000001");

enum class InfoFault
{
    None,
    Timeout,
    Unavailable,
    InvalidPayload,
    ProtocolVersion,
    Unsupported,
    InvalidIdentity,
    Unrelated
};

class RidPeer : public QObject
{
public:
    RidPeer(MockLink* link, Vehicle* vehicle)
        : _link(link)
        , _vehicle(vehicle)
    {
        for (int i = 0; i < 3; ++i) {
            amov_device::Response descriptor;
            descriptor.index = i;
            descriptor.total = 3;
            descriptor.flags = amov_device::WRITABLE | amov_device::DEFAULT_AVAILABLE;
            descriptor.type = i == 2 ? Type::UINT8 : Type::CHAR64;
            descriptor.max_length = i == 2 ? 1 : 64;
            descriptor.maximum = i == 2 ? 1 : 0;
            const QByteArray name = i == 0 ? "WIFI_SSID" : i == 1 ? "WIFI_PASSWORD" : "LOCK_LEVEL";
            std::memcpy(descriptor.name, name.constData(), name.size());
            const QByteArray value = i == 0 ? QByteArray("RID") : i == 1 ? QByteArray("********") : QByteArray(1, '\0');
            descriptor.value_length = value.size();
            std::memcpy(descriptor.value, value.constData(), value.size());
            descriptor.default_length = descriptor.value_length;
            std::memcpy(descriptor.default_value, descriptor.value, descriptor.value_length);
            if (i == 1)
                descriptor.flags |= amov_device::MASKED | amov_device::REBOOT_REQUIRED;
            entries.append(descriptor);
        }
        connect(link, &MockLink::writeBytesQueuedSignal, this, [this](const QByteArray& bytes) {
            mavlink_message_t message{};
            mavlink_status_t status{};
            for (const char byte : bytes) {
                if (mavlink_frame_char_buffer(&_rx, &_rxStatus, static_cast<uint8_t>(byte), &message, &status) ==
                        MAVLINK_FRAMING_OK &&
                    message.msgid == MAVLINK_MSG_ID_AMOV_DEVICE_REQUEST) {
                    handle(message);
                }
            }
        });
    }

    void handle(const mavlink_message_t& message)
    {
        mavlink_amov_device_request_t envelope{};
        mavlink_msg_amov_device_request_decode(&message, &envelope);
        lastEnvelope = envelope;
        if (envelope.protocol_version != 1 || envelope.target_system != _vehicle->id() ||
            envelope.target_component != _vehicle->defaultComponentId() || message.magic != MAVLINK_STX) {
            error = "Invalid transport request";
            return;
        }
        if (envelope.operation == 0) {
            if (holdDiscovery) {
                return;
            }
            send(envelope, discoveredNodes, discoveryStatus);
            return;
        }
        amov_device::Request request;
        if (!amov_device::decode_request(envelope.payload, envelope.payload_length, request)) {
            error = "Request violates shared management codec";
            return;
        }
        requests.append(request);
        const auto infoFault = request.operation == Operation::INFO
                                   ? infoFaults.value(envelope.node_id, InfoFault::None)
                                   : InfoFault::None;
        if (infoFault == InfoFault::Timeout || infoFault == InfoFault::Unavailable) {
            send(envelope, {}, infoFault == InfoFault::Timeout ? 4 : 5);
            return;
        }
        if (infoFault == InfoFault::InvalidPayload) {
            send(envelope, QByteArray("invalid-info"));
            return;
        }
        amov_device::Response response;
        response.operation = request.operation;
        response.transaction = request.transaction;
        response.boot = boot;
        response.total = entries.size();
        response.revision = revision;
        response.activation_state = active;
        std::memcpy(response.uid, uid.constData(), 6);
        if (request.operation != Operation::INFO && QByteArray(reinterpret_cast<char*>(request.uid), 6) != uid) {
            error = "UID mismatch";
            return;
        }
        if (request.boot && request.boot != boot) {
            response.result = Result::CONFLICT;
        } else if (request.operation == Operation::INFO) {
            response.type = Type::CHAR20;
            response.value_length = deviceSerial.size();
            std::memcpy(response.value, deviceSerial.constData(), deviceSerial.size());
            if (provisioningSupported) {
                response.default_length = 1;
                response.default_value[0] = amov_device::FACTORY_PROVISION_CAPABLE;
                if (deviceSerial.isEmpty() && active == 0 && entries[2].value[0] == 0) {
                    response.default_value[0] |= amov_device::FACTORY_PROVISION_AVAILABLE;
                }
            }
        } else if (request.operation == Operation::GET &&
                   (!std::strcmp(request.name, "RID_CLASS") || !std::strcmp(request.name, "RID_ID_REV"))) {
            response.type = Type::UINT32;
            std::strcpy(response.name, request.name);
            response.value_length = 4;
            amov_device::write_u32(response.value,
                                   !std::strcmp(request.name, "RID_CLASS") ? aircraftClass : identityRevision);
        } else if (request.operation == Operation::METADATA || request.operation == Operation::GET) {
            int index = request.operation == Operation::METADATA ? request.index : find(request.name);
            if (index < 0 || index >= entries.size()) {
                error = "Invalid parameter lookup";
                return;
            }
            const auto transaction = response.transaction;
            response = entries[index];
            response.operation = request.operation;
            response.transaction = transaction;
            response.boot = boot;
            response.revision = revision;
            std::memcpy(response.uid, uid.constData(), 6);
        } else if (request.operation == Operation::STATUS) {
            if (statusTransportBusyCount > 0) {
                --statusTransportBusyCount;
                send(envelope, {}, 3);
                return;
            }
            if (unknownReceipt || !ledger.contains(request.transaction))
                response.result = Result::UNKNOWN;
            else if (QByteArray(reinterpret_cast<char*>(request.value), request.value_length) !=
                     hashes.value(request.transaction)) {
                response.result = Result::CONFLICT;
            } else {
                response = ledger.value(request.transaction);
                response.boot = boot;
            }
        } else {
            ++mutations;
            if (request.operation == Operation::PROVISION) {
                if (!deviceSerial.isEmpty() || !provisioningSupported || entries[2].value[0] != 0) {
                    error = "Provisioning was sent to an unavailable device";
                    return;
                }
                deviceSerial = QByteArray(reinterpret_cast<const char*>(request.value + 1), 20);
                active = 0;
                revision = 1;
                aircraftClass = request.value[0] + (corruptProvisionClass ? 1 : 0);
                if (corruptProvisionReadback) {
                    deviceSerial[0] ^= char(1);
                }
                if (wrongProvisionUid) {
                    uid[0] ^= char(1);
                }
            } else if (request.operation == Operation::ACTIVATE) {
                if (request.revision != revision ||
                    QByteArray(reinterpret_cast<char*>(request.value + 1), 20) != serial) {
                    error = "Activation is not bound to current revision/product code";
                    return;
                }
                active = request.value[0];
                ++revision;
            } else if (request.operation == Operation::SET) {
                const auto index = find(request.name);
                if (index < 0 || request.type != entries[index].type) {
                    error = "Invalid SET type/name";
                    return;
                }
                if (index != 1) {
                    entries[index].value_length = request.value_length;
                    std::memcpy(entries[index].value, request.value, request.value_length);
                }
            } else if (request.operation == Operation::RESET) {
                for (auto& entry : entries) {
                    entry.value_length = entry.default_length;
                    std::memcpy(entry.value, entry.default_value, entry.default_length);
                }
            }
            response.result = Result::SAVED;
            response.flags = omitDurability ? 0 : amov_device::DURABLE;
            ledger.insert(request.transaction, response);
            hashes.insert(
                request.transaction,
                QCryptographicHash::hash(QByteArray(reinterpret_cast<char*>(envelope.payload), envelope.payload_length),
                                         QCryptographicHash::Blake2b_256));
            if (request.operation == Operation::REBOOT || request.operation == Operation::RESET)
                ++boot;
            if (corruptReceiptHash) {
                hashes[request.transaction][0] ^= char(1);
            }
            if (afterMutation) {
                afterMutation();
            }
            if (timeoutMutation) {
                timeoutMutation = false;
                send(envelope, {}, 4);
                return;
            }
        }
        if (infoFault == InfoFault::Unsupported) {
            response.result = Result::UNSUPPORTED;
        } else if (infoFault == InfoFault::InvalidIdentity) {
            response.boot = 0;
        } else if (infoFault == InfoFault::Unrelated) {
            ++response.transaction;
        }
        uint8_t bytes[amov_device::RESPONSE_SIZE]{};
        if (!amov_device::encode_response(response, bytes, sizeof(bytes))) {
            error = "Invalid peer response";
            return;
        }
        if (infoFault == InfoFault::ProtocolVersion) {
            bytes[0] = 2;
        }
        if (wrongSessionFirst) {
            wrongSessionFirst = false;
            auto wrong = envelope;
            ++wrong.session_id;
            send(wrong, QByteArray(reinterpret_cast<char*>(bytes), sizeof(bytes)));
        }
        send(envelope, QByteArray(reinterpret_cast<char*>(bytes), sizeof(bytes)));
    }

    int find(const char* name) const
    {
        for (int i = 0; i < entries.size(); ++i)
            if (!std::strcmp(name, entries[i].name))
                return i;
        return -1;
    }

    void send(const mavlink_amov_device_request_t& request, const QByteArray& payload, quint8 result = 0)
    {
        mavlink_amov_device_response_t response{};
        response.session_id = request.session_id;
        response.request_id = request.request_id;
        response.target_system = MAVLinkProtocol::instance()->getSystemId();
        response.target_component = MAVLinkProtocol::getComponentId();
        response.protocol_version = 1;
        response.operation = request.operation;
        response.node_id = request.node_id;
        response.status = result;
        response.payload_length = payload.size();
        std::memcpy(response.payload, payload.constData(), payload.size());
        mavlink_message_t message{};
        mavlink_msg_amov_device_response_encode(_vehicle->id(), _vehicle->defaultComponentId(), &message, &response);
        if (holdResponses) {
            heldResponses.append(message);
        } else {
            _link->respondWithMavlinkMessage(message);
        }
    }

    void releaseResponses()
    {
        holdResponses = false;
        const auto responses = heldResponses;
        heldResponses.clear();
        for (const auto& response : responses) {
            _link->respondWithMavlinkMessage(response);
        }
    }

    MockLink* _link;
    Vehicle* _vehicle;
    mavlink_message_t _rx{};
    mavlink_status_t _rxStatus{};
    mavlink_amov_device_request_t lastEnvelope{};
    QList<amov_device::Request> requests;
    QList<amov_device::Response> entries;
    QHash<quint32, amov_device::Response> ledger;
    QHash<quint32, QByteArray> hashes;
    QByteArray uid = QByteArray::fromHex("001122aabbcc");
    QString error;
    QByteArray deviceSerial = serial;
    bool provisioningSupported = false;
    bool holdDiscovery = false;
    bool holdResponses = false;
    QList<mavlink_message_t> heldResponses;
    QByteArray discoveredNodes = QByteArray(1, char(125));
    quint8 discoveryStatus = 0;
    QHash<quint8, InfoFault> infoFaults;
    bool corruptProvisionReadback = false;
    bool corruptProvisionClass = false;
    uint8_t aircraftClass = 0;
    uint32_t identityRevision = 1;
    bool wrongProvisionUid = false;
    bool omitDurability = false;
    bool unknownReceipt = false;
    std::function<void()> afterMutation;
    quint32 boot = 7;
    quint32 revision = 12;
    quint8 active = 0;
    int mutations = 0;
    int statusTransportBusyCount = 0;
    bool timeoutMutation = false;
    bool corruptReceiptHash = false;
    bool wrongSessionFirst = false;
};

}  // namespace

void VehicleRidManagerTest::_parameterRoundTripAndValidation()
{
    RidPeer peer(mockLink(), vehicle());
    auto* manager = vehicle()->ridManager();
    mavlink_get_channel_status(mockLink()->mavlinkChannel())->flags |= MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
    manager->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(manager->supported() && !manager->busy(), TestTimeout::mediumMs());
    QCOMPARE(manager->parameters().size(), 3);
    QCOMPARE(manager->serial().toLatin1(), serial);
    QCOMPARE(peer.error, QString());
    peer.timeoutMutation = true;
    peer.statusTransportBusyCount = 2;
    manager->writeParameter("WIFI_SSID", "新的 RID");
    QTRY_VERIFY_WITH_TIMEOUT(!manager->busy(), TestTimeout::mediumMs());
    QCOMPARE(peer.mutations, 1);
    QCOMPARE(manager->_parameters.find("WIFI_SSID")->fact()->rawValue().toString(), QString("新的 RID"));
    QVERIFY(std::any_of(peer.requests.begin(), peer.requests.end(),
                        [](const auto& request) { return request.operation == Operation::STATUS; }));
    manager->writeParameter("WIFI_PASSWORD", "********");
    QVERIFY(!manager->busy());
    QCOMPARE(peer.mutations, 1);
    manager->writeParameter("WIFI_PASSWORD", "new-password");
    QTRY_VERIFY_WITH_TIMEOUT(!manager->busy(), TestTimeout::mediumMs());
    QCOMPARE(peer.mutations, 2);
    QCOMPARE(manager->_parameters.find("WIFI_PASSWORD")->fact()->rawValue().toString(), QString("********"));
    QVERIFY(manager->_parameterValue.isEmpty());
    QVERIFY(manager->_mutationDigest.isEmpty());
    QCOMPARE(QByteArray(reinterpret_cast<char*>(manager->_mutation.value), 64), QByteArray(64, '\0'));
    QCOMPARE(peer.error, QString());
}

void VehicleRidManagerTest::_activationRoundTrip_data()
{
    QTest::addColumn<bool>("active");
    QTest::addColumn<bool>("receiptRecovery");
    QTest::newRow("activate") << true << false;
    QTest::newRow("deactivate") << false << false;
    QTest::newRow("activate-recover-receipt") << true << true;
    QTest::newRow("deactivate-recover-receipt") << false << true;
}

void VehicleRidManagerTest::_activationRoundTrip()
{
    QFETCH(bool, active);
    QFETCH(bool, receiptRecovery);
    RidPeer peer(mockLink(), vehicle());
    peer.active = active ? 0 : 1;
    peer.timeoutMutation = receiptRecovery;
    auto* manager = vehicle()->ridManager();
    QSignalSpy read(manager, &VehicleRidManager::activationRead);
    QSignalSpy written(manager, &VehicleRidManager::activationWritten);
    QSignalSpy failed(manager, &VehicleRidManager::activationFailed);
    manager->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(manager->supported() && !manager->busy(), TestTimeout::mediumMs());
    QVERIFY(manager->readActivation(10));
    QTRY_COMPARE_WITH_TIMEOUT(read.count(), 1, TestTimeout::mediumMs());
    QCOMPARE(read.at(0).at(0).toULongLong(), quint64(10));
    QCOMPARE(read.at(0).at(2).toInt(), active ? 0 : 1);
    QCOMPARE(peer.mutations, 0);
    const auto context = qvariant_cast<RidActivationContext>(read.at(0).at(1));
    QVERIFY(context == manager->activationContext());
    QVERIFY(manager->setActivation(11, context, active));
    QTRY_COMPARE_WITH_TIMEOUT(written.count(), 1, TestTimeout::mediumMs());
    QCOMPARE(written.at(0).at(0).toULongLong(), quint64(11));
    QCOMPARE(written.at(0).at(2).toInt(), active ? 1 : 0);
    QVERIFY(qvariant_cast<RidActivationContext>(written.at(0).at(1)) == manager->activationContext());
    QCOMPARE(manager->activationState(), active ? 1 : 0);
    QCOMPARE(peer.mutations, 1);
    QCOMPARE(peer.error, QString());
    QCOMPARE(failed.count(), 0);
    QCOMPARE(manager->activationContext().revision, quint32(13));
    QVERIFY(!manager->busy());
}

void VehicleRidManagerTest::_activationIdentityGuard_data()
{
    QTest::addColumn<QString>("field");
    QTest::addColumn<bool>("deviceChanged");
    for (const char* field : {"serial", "uid", "boot", "revision", "generation"}) {
        QTest::newRow(qPrintable(QStringLiteral("stale-%1").arg(field))) << QString::fromLatin1(field) << false;
        if (std::strcmp(field, "generation")) {
            QTest::newRow(qPrintable(QStringLiteral("device-%1").arg(field))) << QString::fromLatin1(field) << true;
        }
    }
}

void VehicleRidManagerTest::_activationIdentityGuard()
{
    QFETCH(QString, field);
    QFETCH(bool, deviceChanged);
    RidPeer peer(mockLink(), vehicle());
    auto* manager = vehicle()->ridManager();
    manager->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(manager->supported() && !manager->busy(), TestTimeout::mediumMs());
    auto context = manager->activationContext();
    if (deviceChanged) {
        if (field == "serial") {
            peer.deviceSerial[0] = 'X';
        } else if (field == "uid") {
            peer.uid[0] = '\x22';
        } else if (field == "boot") {
            ++peer.boot;
        } else if (field == "revision") {
            ++peer.revision;
        }
    } else if (field == "serial") {
        context.serial[0] = u'X';
    } else if (field == "uid") {
        context.uid[0] = u'F';
    } else if (field == "boot") {
        ++context.boot;
    } else if (field == "revision") {
        ++context.revision;
    } else {
        ++context.generation;
    }
    QSignalSpy failed(manager, &VehicleRidManager::activationFailed);
    QSignalSpy written(manager, &VehicleRidManager::activationWritten);
    QCOMPARE(manager->setActivation(12, context, true), deviceChanged);
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, TestTimeout::mediumMs());
    QCOMPARE(failed.at(0).at(0).toULongLong(), quint64(12));
    QVERIFY(!failed.at(0).at(2).toBool());
    QCOMPARE(written.count(), 0);
    QCOMPARE(peer.mutations, 0);
    QVERIFY(!manager->busy());
}

void VehicleRidManagerTest::_activationOutcomeFailure_data()
{
    QTest::addColumn<QString>("failure");
    QTest::addColumn<bool>("uncertain");
    QTest::newRow("no-durable-receipt") << QStringLiteral("durability") << true;
    QTest::newRow("readback-mismatch") << QStringLiteral("readback") << true;
    QTest::newRow("disconnect-after-write") << QStringLiteral("disconnect") << true;
    QTest::newRow("read-failed") << QStringLiteral("read") << false;
}

void VehicleRidManagerTest::_activationOutcomeFailure()
{
    QFETCH(QString, failure);
    QFETCH(bool, uncertain);
    RidPeer peer(mockLink(), vehicle());
    auto* manager = vehicle()->ridManager();
    manager->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(manager->supported() && !manager->busy(), TestTimeout::mediumMs());
    QSignalSpy failed(manager, &VehicleRidManager::activationFailed);
    QSignalSpy written(manager, &VehicleRidManager::activationWritten);
    QSignalSpy read(manager, &VehicleRidManager::activationRead);
    if (failure == "durability") {
        peer.omitDurability = true;
    } else if (failure == "readback") {
        peer.afterMutation = [&peer]() { peer.active = 0; };
    } else if (failure == "disconnect") {
        peer.afterMutation = [manager]() { manager->_resetConnection(); };
    } else {
        peer.infoFaults.insert(125, InfoFault::Timeout);
    }
    if (failure == "read") {
        QVERIFY(manager->readActivation(13));
    } else {
        QVERIFY(manager->setActivation(13, manager->activationContext(), true));
    }
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, TestTimeout::mediumMs());
    QCOMPARE(failed.at(0).at(0).toULongLong(), quint64(13));
    QCOMPARE(failed.at(0).at(2).toBool(), uncertain);
    QCOMPARE(written.count(), 0);
    QCOMPARE(read.count(), 0);
    QVERIFY(!manager->busy());
    QCoreApplication::processEvents();
    QCOMPARE(failed.count(), 1);
    QCOMPARE(written.count(), 0);
    QCOMPARE(peer.error, QString());
}

void VehicleRidManagerTest::_activationConcurrentRequestIsolation()
{
    RidPeer peer(mockLink(), vehicle());
    auto* manager = vehicle()->ridManager();
    manager->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(manager->supported() && !manager->busy(), TestTimeout::mediumMs());
    QSignalSpy read(manager, &VehicleRidManager::activationRead);
    QSignalSpy written(manager, &VehicleRidManager::activationWritten);
    QSignalSpy failed(manager, &VehicleRidManager::activationFailed);
    peer.holdResponses = true;
    QVERIFY(manager->readActivation(101));
    QTRY_COMPARE_WITH_TIMEOUT(peer.heldResponses.size(), 1, TestTimeout::mediumMs());
    QVERIFY(manager->busy());
    QCOMPARE(read.count(), 0);
    QVERIFY(!manager->readActivation(102));
    QCOMPARE(failed.count(), 1);
    QCOMPARE(failed.at(0).at(0).toULongLong(), quint64(102));
    peer.releaseResponses();
    QTRY_COMPARE_WITH_TIMEOUT(read.count(), 1, TestTimeout::mediumMs());
    QCOMPARE(read.at(0).at(0).toULongLong(), quint64(101));
    const auto context = qvariant_cast<RidActivationContext>(read.at(0).at(1));
    peer.holdResponses = true;
    QVERIFY(manager->setActivation(103, context, true));
    QTRY_COMPARE_WITH_TIMEOUT(peer.heldResponses.size(), 1, TestTimeout::mediumMs());
    QVERIFY(manager->busy());
    QCOMPARE(written.count(), 0);
    QVERIFY(!manager->setActivation(104, context, false));
    QCOMPARE(failed.count(), 2);
    QCOMPARE(failed.at(1).at(0).toULongLong(), quint64(104));
    peer.releaseResponses();
    QTRY_COMPARE_WITH_TIMEOUT(written.count(), 1, TestTimeout::mediumMs());
    QCOMPARE(written.at(0).at(0).toULongLong(), quint64(103));
    QCOMPARE(written.at(0).at(2).toInt(), 1);
    QCOMPARE(manager->activationState(), 1);
    QCOMPARE(peer.mutations, 1);
    QCOMPARE(peer.error, QString());
}

void VehicleRidManagerTest::_identityAndSessionIsolation()
{
    RidPeer peer(mockLink(), vehicle());
    auto* manager = vehicle()->ridManager();
    peer.wrongSessionFirst = true;
    manager->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(manager->supported() && !manager->busy(), TestTimeout::mediumMs());
    const QString previousUid = manager->uid();
    peer.uid[0] = '\x22';
    manager->writeParameter("WIFI_SSID", "must-not-save");
    QTRY_VERIFY_WITH_TIMEOUT(!manager->busy(), TestTimeout::mediumMs());
    QCOMPARE(peer.mutations, 0);
    QVERIFY(!manager->supported());
    QCOMPARE(manager->uid(), previousUid);
    manager->_verifyDefaults = true;
    manager->_fail(QStringLiteral("interrupted reset verification"));
    QVERIFY(!manager->_verifyDefaults);
    manager->_verifyDefaults = true;
    manager->_resetConnection();
    QVERIFY(!manager->_verifyDefaults);
}

void VehicleRidManagerTest::_maintenanceAndGroundRestriction()
{
    RidPeer peer(mockLink(), vehicle());
    auto* manager = vehicle()->ridManager();
    manager->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(manager->supported() && !manager->busy(), TestTimeout::mediumMs());
    manager->writeParameter("WIFI_SSID", "changed");
    QTRY_VERIFY_WITH_TIMEOUT(!manager->busy(), TestTimeout::mediumMs());
    manager->resetDefaults();
    QTRY_VERIFY_WITH_TIMEOUT(!manager->busy(), TestTimeout::mediumMs());
    QCOMPARE(manager->_boot, quint32(8));
    QCOMPARE(manager->_parameters.find("WIFI_SSID")->fact()->rawValue().toString(), QString("RID"));
    QCOMPARE(manager->serial().toLatin1(), serial);
    QVERIFY(manager->_parameters.defaultsMatch());
    const int writes = peer.mutations;
    const auto beforeArming = manager->activationContext();
    mockLink()->setArmed(true);
    QTRY_VERIFY_WITH_TIMEOUT(vehicle()->armed(), TestTimeout::mediumMs());
    QVERIFY(!manager->canWrite());
    manager->writeParameter("WIFI_SSID", "blocked");
    QVERIFY(!manager->setActivation(14, manager->activationContext(), true));
    QVERIFY(!manager->readActivation(15));
    manager->resetDefaults();
    QCOMPARE(peer.mutations, writes);
    mockLink()->setArmed(false);
    QTRY_VERIFY_WITH_TIMEOUT(!vehicle()->armed(), TestTimeout::mediumMs());
    QVERIFY(!manager->setActivation(16, beforeArming, true));
    QCOMPARE(peer.mutations, writes);
    QCOMPARE(peer.error, QString());
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg, QRegularExpression("Failed to connect to pressure backend"));
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg, QRegularExpression("Error Initializing Pressure Sensor"));
    mavlink_message_t flight{};
    mavlink_msg_extended_sys_state_pack(vehicle()->id(), vehicle()->defaultComponentId(), &flight,
                                        MAV_VTOL_STATE_UNDEFINED, MAV_LANDED_STATE_IN_AIR);
    mockLink()->respondWithMavlinkMessage(flight);
    QTRY_VERIFY_WITH_TIMEOUT(vehicle()->underway(), TestTimeout::mediumMs());
    QVERIFY(!vehicle()->armed());
    QVERIFY(!manager->canWrite());
    manager->writeParameter("WIFI_SSID", "blocked-underway");
    QCOMPARE(peer.mutations, writes);
    mavlink_msg_extended_sys_state_pack(vehicle()->id(), vehicle()->defaultComponentId(), &flight,
                                        MAV_VTOL_STATE_UNDEFINED, MAV_LANDED_STATE_ON_GROUND);
    mockLink()->respondWithMavlinkMessage(flight);
    QTRY_VERIFY_WITH_TIMEOUT(!vehicle()->underway(), TestTimeout::mediumMs());
}

void VehicleRidManagerTest::_parameterValueBoundaries()
{
    amov_device::Response descriptor;
    descriptor.type = Type::CHAR20;
    descriptor.flags = amov_device::WRITABLE;
    descriptor.max_length = 20;
    std::strcpy(descriptor.name, "UA_TYPE_NAME");
    RidParameterEntry text(descriptor);
    QByteArray encoded;
    QVERIFY(text.encodeValue(QString(7, QChar(0x4e2d)), encoded).size() > 0);
    QVERIFY(text.encodeValue(QString(6, QChar(0x4e2d)), encoded).isEmpty());
    QCOMPARE(encoded.size(), 18);
    descriptor.type = Type::FLOAT;
    descriptor.minimum = -1;
    descriptor.maximum = 1;
    descriptor.value_length = 4;
    RidParameterEntry number(descriptor);
    QVERIFY(!number.encodeValue("nan", encoded).isEmpty());
    QVERIFY(!number.encodeValue("inf", encoded).isEmpty());
    QVERIFY(!number.encodeValue("1.1", encoded).isEmpty());
    QVERIFY(number.encodeValue("-0.5", encoded).isEmpty());
    QCOMPARE(amov_device::read_float(reinterpret_cast<const uint8_t*>(encoded.constData())), -0.5f);
    descriptor.flags = 0;
    RidParameterEntry readOnly(descriptor);
    QVERIFY(!readOnly.encodeValue("0", encoded).isEmpty());
}

void VehicleRidManagerTest::_credentialRequestsAreNotLogged()
{
    auto* protocol = MAVLinkProtocol::instance();
    QTemporaryFile log;
    QVERIFY(log.open());
    auto* original = protocol->_tempLogFile;
    const bool suspendedError = protocol->_logSuspendError;
    const bool suspendedReplay = protocol->_logSuspendReplay;
    const auto restore = qScopeGuard([&]() {
        protocol->_tempLogFile = original;
        protocol->_logSuspendError = suspendedError;
        protocol->_logSuspendReplay = suspendedReplay;
    });
    protocol->_tempLogFile = &log;
    protocol->_logSuspendError = false;
    protocol->_logSuspendReplay = false;
    mavlink_amov_device_request_t request{};
    request.protocol_version = 1;
    request.payload_length = 8;
    std::memcpy(request.payload, "password", 8);
    mavlink_message_t message{};
    mavlink_msg_amov_device_request_encode(255, 190, &message, &request);
    uint8_t buffer[MAVLINK_MAX_PACKET_LEN]{};
    auto length = mavlink_msg_to_send_buffer(buffer, &message);
    protocol->logSentBytes(mockLink(), QByteArray(reinterpret_cast<char*>(buffer), length));
    protocol->_logData(mockLink(), message);
    QCOMPARE(log.size(), qint64(0));
    mavlink_amov_rid_identity_request_t identity{};
    mavlink_msg_amov_rid_identity_request_encode(255, 190, &message, &identity);
    length = mavlink_msg_to_send_buffer(buffer, &message);
    protocol->logSentBytes(mockLink(), QByteArray(reinterpret_cast<char*>(buffer), length));
    QVERIFY(log.size() > 0);
    const auto previous = log.size();
    protocol->_logData(mockLink(), message);
    QVERIFY(log.size() > previous);
}

void VehicleRidManagerTest::_statusDigestBinding()
{
    amov_device::Request reference;
    reference.operation = Operation::SET;
    reference.type = Type::CHAR20;
    reference.transaction = 0x12345678;
    reference.boot = 0x90abcdef;
    const auto uid = QByteArray::fromHex("001122334455");
    std::memcpy(reference.uid, uid.constData(), 6);
    std::strcpy(reference.name, "WIFI_PASSWORD");
    reference.value_length = 8;
    std::memcpy(reference.value, "abcdefgh", 8);
    uint8_t bytes[amov_device::REQUEST_SIZE]{};
    QVERIFY(amov_device::encode_request(reference, bytes, sizeof(bytes)));
    QFile vectorFile(QFINDTESTDATA("../../../protocol/amov_device_bridge/status_digest_vector.json"));
    QVERIFY(vectorFile.open(QIODevice::ReadOnly));
    const auto vector = QJsonDocument::fromJson(vectorFile.readAll()).object();
    QCOMPARE(QByteArray(reinterpret_cast<char*>(bytes), sizeof(bytes)).toHex(),
             vector.value("request_hex").toString().toLatin1());
    QCOMPARE(QCryptographicHash::hash(QByteArrayView(reinterpret_cast<char*>(bytes), sizeof(bytes)),
                                      QCryptographicHash::Blake2b_256)
                 .toHex(),
             vector.value("blake2b_256").toString().toLatin1());
    RidPeer peer(mockLink(), vehicle());
    auto* manager = vehicle()->ridManager();
    manager->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(manager->supported() && !manager->busy(), TestTimeout::mediumMs());
    peer.timeoutMutation = true;
    peer.corruptReceiptHash = true;
    manager->writeParameter("WIFI_PASSWORD", "new-password");
    QTRY_VERIFY_WITH_TIMEOUT(!manager->busy(), TestTimeout::mediumMs());
    QCOMPARE(peer.mutations, 1);
    QVERIFY(manager->statusText().contains("state changed"));
    QVERIFY(manager->_mutationDigest.isEmpty());
    QCOMPARE(peer.error, QString());
}

void VehicleRidManagerTest::_factoryProvisionRoundTrip_data()
{
    QTest::addColumn<QString>("mode");
    QTest::newRow("direct") << QStringLiteral("direct");
    QTest::newRow("lost-receipt") << QStringLiteral("lost-receipt");
    QTest::newRow("connection-reset") << QStringLiteral("connection-reset");
}

void VehicleRidManagerTest::_factoryProvisionRoundTrip()
{
    QFETCH(QString, mode);
    RidPeer peer(mockLink(), vehicle());
    peer.deviceSerial.clear();
    peer.provisioningSupported = true;
    peer.revision = 0;
    auto* manager = vehicle()->ridManager();
    manager->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(manager->canProvision(), TestTimeout::mediumMs());
    const QString targetUid = manager->uid();
    if (mode == QStringLiteral("lost-receipt")) {
        peer.timeoutMutation = true;
        peer.statusTransportBusyCount = 2;
    } else if (mode == QStringLiteral("connection-reset")) {
        peer.afterMutation = [manager]() { manager->_resetConnection(); };
    }
    manager->provisionFactoryIdentity(targetUid, QString::fromLatin1(serial), 2);
    if (mode == QStringLiteral("connection-reset")) {
        QTRY_VERIFY_WITH_TIMEOUT(!manager->busy(), TestTimeout::mediumMs());
        QVERIFY(manager->statusText().contains("outcome is unknown"));
        QVERIFY(!manager->canProvision());
        QVERIFY(!manager->canWrite());
        peer.afterMutation = {};
        manager->refresh();
    }
    QTRY_VERIFY_WITH_TIMEOUT(!manager->busy(), TestTimeout::mediumMs());
    QCOMPARE(peer.error, QString());
    QCOMPARE(peer.mutations, 1);
    QCOMPARE(manager->uid(), targetUid);
    QCOMPARE(manager->serial().toLatin1(), serial);
    QCOMPARE(manager->activationState(), 0);
    QVERIFY(manager->statusText().contains("saved and verified"));
    QVERIFY(!manager->canProvision());
    QVERIFY(manager->canWrite());
    QCOMPARE(manager->_parameters.find("WIFI_SSID")->fact()->rawValue().toString(), QStringLiteral("RID"));
    QCOMPARE(manager->_parameters.find("LOCK_LEVEL")->fact()->rawValue().toInt(), 0);
    manager->provisionFactoryIdentity(targetUid, QStringLiteral("OTHER-AIRCRAFT-00001"), 3);
    QCOMPARE(peer.mutations, 1);
    int provisionRequests = 0;
    for (const auto& request : peer.requests) {
        if (request.operation == Operation::PROVISION) {
            ++provisionRequests;
            QCOMPARE(request.value[0], uint8_t(2));
            QCOMPARE(QByteArray(reinterpret_cast<const char*>(request.value + 1), 20), serial);
        }
    }
    QCOMPARE(provisionRequests, 1);
}

void VehicleRidManagerTest::_factoryProvisionRefusal_data()
{
    QTest::addColumn<QString>("condition");
    for (const char* condition : {"unsupported", "provisioned", "locked", "armed", "wrong-uid", "short-sn", "space-sn",
                                  "unicode-sn", "invalid-class", "busy"}) {
        QTest::newRow(condition) << QString::fromLatin1(condition);
    }
}

void VehicleRidManagerTest::_factoryProvisionRefusal()
{
    QFETCH(QString, condition);
    RidPeer peer(mockLink(), vehicle());
    peer.deviceSerial = condition == QStringLiteral("provisioned") ? serial : QByteArray();
    peer.provisioningSupported = condition != QStringLiteral("unsupported");
    peer.entries[2].value[0] = condition == QStringLiteral("locked") ? 1 : 0;
    auto* manager = vehicle()->ridManager();
    manager->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(manager->supported() && !manager->busy(), TestTimeout::mediumMs());
    QString productCode = QString::fromLatin1(serial);
    QString targetUid = manager->uid();
    int aircraftClass = 2;
    if (condition == QStringLiteral("armed")) {
        mockLink()->setArmed(true);
        QTRY_VERIFY_WITH_TIMEOUT(vehicle()->armed(), TestTimeout::mediumMs());
    } else if (condition == QStringLiteral("wrong-uid")) {
        targetUid = QStringLiteral("000000000001");
    } else if (condition == QStringLiteral("short-sn")) {
        productCode.chop(1);
    } else if (condition == QStringLiteral("space-sn")) {
        productCode[3] = QChar(' ');
    } else if (condition == QStringLiteral("unicode-sn")) {
        productCode[3] = QChar(0x4e2d);
    } else if (condition == QStringLiteral("invalid-class")) {
        aircraftClass = 5;
    } else if (condition == QStringLiteral("busy")) {
        peer.holdDiscovery = true;
        manager->refresh();
        QVERIFY(manager->busy());
    }
    manager->provisionFactoryIdentity(targetUid, productCode, aircraftClass);
    if (peer.holdDiscovery) {
        peer.holdDiscovery = false;
        peer.send(peer.lastEnvelope, QByteArray(1, char(125)));
    }
    QTRY_VERIFY_WITH_TIMEOUT(!manager->busy(), TestTimeout::mediumMs());
    QCOMPARE(peer.mutations, 0);
    QCOMPARE(peer.error, QString());
    if (vehicle()->armed()) {
        mockLink()->setArmed(false);
        QTRY_VERIFY_WITH_TIMEOUT(!vehicle()->armed(), TestTimeout::mediumMs());
    }
}

void VehicleRidManagerTest::_factoryProvisionVerification_data()
{
    QTest::addColumn<QString>("fault");
    for (const char* fault : {"serial", "class", "identity-version", "uid", "durability", "receipt"}) {
        QTest::newRow(fault) << QString::fromLatin1(fault);
    }
}

void VehicleRidManagerTest::_factoryProvisionVerification()
{
    QFETCH(QString, fault);
    RidPeer peer(mockLink(), vehicle());
    peer.deviceSerial.clear();
    peer.provisioningSupported = true;
    peer.revision = 0;
    auto* manager = vehicle()->ridManager();
    manager->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(manager->canProvision(), TestTimeout::mediumMs());
    peer.corruptProvisionReadback = fault == QStringLiteral("serial");
    peer.corruptProvisionClass = fault == QStringLiteral("class");
    peer.identityRevision = fault == QStringLiteral("identity-version") ? 2 : 1;
    const auto clearReceipt = qScopeGuard([manager]() { manager->_clearProvisionReceipt(); });
    peer.wrongProvisionUid = fault == QStringLiteral("uid");
    peer.omitDurability = fault == QStringLiteral("durability");
    peer.unknownReceipt = fault == QStringLiteral("receipt");
    peer.timeoutMutation = peer.unknownReceipt;
    manager->provisionFactoryIdentity(manager->uid(), QString::fromLatin1(serial), 2);
    QTRY_VERIFY_WITH_TIMEOUT(!manager->busy(), TestTimeout::mediumMs());
    QCOMPARE(peer.mutations, 1);
    QVERIFY(!manager->statusText().contains("saved and verified"));
    QVERIFY(manager->statusText().contains("outcome is unknown"));
    QVERIFY(!manager->canWrite());
    QVERIFY(!manager->canProvision());
    manager->provisionFactoryIdentity(manager->uid(), QString::fromLatin1(serial), 2);
    QCOMPARE(peer.mutations, 1);
    QCOMPARE(peer.error, QString());
}

void VehicleRidManagerTest::_factoryProvisionManagerRecreation()
{
    RidPeer peer(mockLink(), vehicle());
    peer.deviceSerial.clear();
    peer.provisioningSupported = true;
    peer.revision = 0;
    auto manager = std::make_unique<VehicleRidManager>(vehicle());
    connect(vehicle(), &Vehicle::mavlinkMessageReceived, manager.get(), &VehicleRidManager::mavlinkMessageReceived);
    manager->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(manager->canProvision(), TestTimeout::mediumMs());
    const QString targetUid = manager->uid();
    peer.afterMutation = [&manager]() { manager->_resetConnection(); };
    manager->provisionFactoryIdentity(targetUid, QString::fromLatin1(serial), 2);
    QTRY_VERIFY_WITH_TIMEOUT(!manager->busy(), TestTimeout::mediumMs());
    QVERIFY(manager->statusText().contains("outcome is unknown"));
    manager.reset();
    peer.afterMutation = {};
    manager = std::make_unique<VehicleRidManager>(vehicle());
    connect(vehicle(), &Vehicle::mavlinkMessageReceived, manager.get(), &VehicleRidManager::mavlinkMessageReceived);
    manager->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(!manager->busy(), TestTimeout::mediumMs());
    QCOMPARE(manager->uid(), targetUid);
    QCOMPARE(manager->serial().toLatin1(), serial);
    QCOMPARE(manager->activationState(), 0);
    QVERIFY(manager->statusText().contains("saved and verified"));
    QCOMPARE(peer.mutations, 1);
    QVERIFY(std::any_of(peer.requests.cbegin(), peer.requests.cend(),
                        [](const auto& request) { return request.operation == Operation::STATUS; }));
    QCOMPARE(peer.error, QString());
}

void VehicleRidManagerTest::_discoveryDiagnostics_data()
{
    QTest::addColumn<QString>("scenario");
    QTest::addColumn<QStringList>("details");
    QTest::newRow("empty-list") << QStringLiteral("empty-list") << QStringList{"did not discover any online CAN nodes"};
    QTest::newRow("timeout") << QStringLiteral("timeout") << QStringList{"CAN node 125", "INFO request timed out"};
    QTest::newRow("unavailable") << QStringLiteral("unavailable") << QStringList{"CAN node 125", "unavailable"};
    QTest::newRow("invalid-payload") << QStringLiteral("invalid-payload")
                                     << QStringList{"CAN node 125", "Invalid or unrelated"};
    QTest::newRow("protocol-version") << QStringLiteral("protocol-version")
                                      << QStringList{"CAN node 125", "Unsupported RID management protocol version 2"};
    QTest::newRow("unsupported") << QStringLiteral("unsupported")
                                 << QStringList{"CAN node 125", "does not support the operation"};
    QTest::newRow("invalid-identity") << QStringLiteral("invalid-identity")
                                      << QStringList{"CAN node 125", "Invalid RID identity response"};
    QTest::newRow("unrelated") << QStringLiteral("unrelated") << QStringList{"CAN node 125", "Invalid or unrelated"};
    QTest::newRow("missing-bridge") << QStringLiteral("missing-bridge")
                                    << QStringList{"Flight-controller firmware does not support RID management"};
    QTest::newRow("list-timeout") << QStringLiteral("list-timeout")
                                  << QStringList{"flight controller did not answer the CAN-node discovery request"};
    QTest::newRow("invalid-list") << QStringLiteral("invalid-list")
                                  << QStringList{"flight controller returned an invalid CAN node list"};
    QTest::newRow("mixed") << QStringLiteral("mixed")
                           << QStringList{"RID parameters read", "CAN node 124", "INFO request timed out"};
    QTest::newRow("multiple-failures") << QStringLiteral("multiple-failures")
                                       << QStringList{"CAN node 124", "INFO request timed out", "CAN node 125",
                                                      "Invalid or unrelated"};
}

void VehicleRidManagerTest::_discoveryDiagnostics()
{
    QFETCH(QString, scenario);
    QFETCH(QStringList, details);
    RidPeer peer(mockLink(), vehicle());
    const QHash<QString, InfoFault> faults{{"timeout", InfoFault::Timeout},
                                           {"unavailable", InfoFault::Unavailable},
                                           {"invalid-payload", InfoFault::InvalidPayload},
                                           {"protocol-version", InfoFault::ProtocolVersion},
                                           {"unsupported", InfoFault::Unsupported},
                                           {"invalid-identity", InfoFault::InvalidIdentity},
                                           {"unrelated", InfoFault::Unrelated}};
    peer.infoFaults.insert(125, faults.value(scenario, InfoFault::None));
    if (scenario == QStringLiteral("empty-list")) {
        peer.discoveredNodes.clear();
    } else if (scenario == QStringLiteral("missing-bridge")) {
        peer.discoveryStatus = 1;
    } else if (scenario == QStringLiteral("list-timeout")) {
        peer.discoveryStatus = 4;
    } else if (scenario == QStringLiteral("invalid-list")) {
        peer.discoveredNodes = QByteArray(1, char(255));
    } else if (scenario == QStringLiteral("mixed") || scenario == QStringLiteral("multiple-failures")) {
        peer.discoveredNodes = QByteArray::fromHex("7c7d");
        peer.infoFaults.insert(124, InfoFault::Timeout);
        if (scenario == QStringLiteral("multiple-failures")) {
            peer.infoFaults.insert(125, InfoFault::InvalidPayload);
        }
    }
    auto* manager = vehicle()->ridManager();
    manager->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(!manager->busy(), TestTimeout::mediumMs());
    for (const auto& detail : details) {
        QVERIFY2(manager->statusText().contains(detail), qPrintable(manager->statusText()));
    }
    QCOMPARE(manager->supported(), scenario == QStringLiteral("mixed"));
    QVERIFY(!manager->statusText().contains("Older RID"));
    QVERIFY(!manager->statusText().contains("signed activation"));
    QCOMPARE(peer.mutations, 0);
    QCOMPARE(peer.error, QString());
    peer.discoveryStatus = 0;
    peer.discoveredNodes = QByteArray(1, char(125));
    peer.infoFaults.clear();
    manager->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(manager->supported() && !manager->busy(), TestTimeout::mediumMs());
    QCOMPARE(manager->statusText(), QStringLiteral("RID parameters read"));
}
