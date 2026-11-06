#include <memory>

#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QPointer>
#include <QtCore/QSemaphore>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtSql/QSqlQuery>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include "UomReportingService.h"

using namespace UomReporting;

namespace {
// Standalone worker tests use bounded, event-driven socket waits without linking the application harness.
constexpr int TEST_TIMEOUT_MS = 5000;
const QString AIRCRAFT = QStringLiteral("1581F45T0123456789AB");

class Fixture : public QObject
{
public:
    Fixture()
    {
        connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (auto* socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer = QByteArray()]() mutable {
                    if (socket->property("handled").toBool()) {
                        return;
                    }
                    buffer += socket->readAll();
                    const auto boundary = buffer.indexOf("\r\n\r\n");
                    if (boundary < 0) {
                        return;
                    }
                    int length = -1;
                    for (const auto& line : buffer.left(boundary).split('\n')) {
                        if (line.toLower().startsWith("content-length:")) {
                            length = line.mid(15).trimmed().toInt();
                        }
                    }
                    if (length < 0 || buffer.size() < boundary + 4 + length) {
                        return;
                    }
                    socket->setProperty("handled", true);
                    sockets.append(socket);
                    bodies.append(buffer.mid(boundary + 4, length));
                    if (!hold) {
                        respond(sockets.size() - 1);
                    }
                });
            }
        });
    }

    Profile profile() const
    {
        return {.mode = Mode::Isolated,
                .endpoint = QUrl(QStringLiteral("http://127.0.0.1:%1/ridData").arg(server.serverPort())),
                .appId = QStringLiteral("offline-app"),
                .uasModel = QStringLiteral("TEST-MODEL"),
                .uscCode = QStringLiteral("TEST-USC"),
                .platform = QStringLiteral("QGC-test"),
                .credentialReference = QStringLiteral("uom-reporting/offline")};
    }

    Capture capture() const
    {
        Capture result;
        result.profile = profile();
        result.sample.aircraftId = AIRCRAFT;
        result.sample.operationState = 2;
        result.sample.timestampMs = QDateTime::currentMSecsSinceEpoch();
        result.identity = {{"uid", "001122AABBCC"}, {"revision", 1}};
        result.bootId = 123;
        result.flightSequence = 1;
        result.takeoffUtcMs = result.sample.timestampMs - 1000;
        result.connection = 10;
        result.airborne = result.valid = true;
        return result;
    }

    void respond(qsizetype index)
    {
        const auto socket = sockets.value(index);
        if (socket && socket->state() == QAbstractSocket::ConnectedState) {
            const QByteArray response = "{\"code\":" + QByteArray::number(code) + ",\"msg\":\"test\"}";
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                          QByteArray::number(response.size()) + "\r\nConnection: close\r\n\r\n" + response);
            socket->disconnectFromHost();
        }
    }

    QTcpServer server;
    QList<QPointer<QTcpSocket>> sockets;
    QList<QByteArray> bodies;
    bool hold = false;
    int code = 200;
};
}  // namespace

class UomReportingServiceTest : public QObject
{
    Q_OBJECT
    void configure(UomReportingService& service, const Profile& profile)
    {
        const UomReportingService::Binding binding{.profile = profile, .aircraftIds = {AIRCRAFT}};
        service._configureIsolated({binding}, {{profile.credentialReference, QStringLiteral("test-key")}});
    }

private slots:

    void liveAcknowledgementAndDisconnect()
    {
        Fixture fixture;
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost, 0));
        QTemporaryDir directory;
        UomReportingService service(directory.filePath(QStringLiteral("reports.sqlite")));
        configure(service, fixture.profile());
        QTRY_VERIFY_WITH_TIMEOUT(service.ready(), TEST_TIMEOUT_MS);
        QVERIFY(service.credentialAvailable(fixture.profile().credentialReference));
        const auto capture = fixture.capture();
        QVERIFY(!service.status(AIRCRAFT).value("healthy").toBool());
        service.capture(capture);
        QTRY_VERIFY_WITH_TIMEOUT(service.status(AIRCRAFT).value("healthy").toBool(), TEST_TIMEOUT_MS);
        QCOMPARE(service.status(AIRCRAFT).value("accepted").toInt(), 1);
        QCOMPARE(fixture.bodies.size(), 1);
        service.disconnectAircraft(AIRCRAFT, capture.connection);
        QTRY_VERIFY_WITH_TIMEOUT(!service.status(AIRCRAFT).value("healthy").toBool(), TEST_TIMEOUT_MS);
        QVERIFY(service.ready());
        service.shutdown();
        QVERIFY(!service.ready());
    }

    void staleAcknowledgementCannotRestoreCurrentSession_data()
    {
        QTest::addColumn<QString>("change");
        for (const auto* name : {"boot", "identity", "connection", "flight", "model", "timestamp", "ground"}) {
            QTest::newRow(name) << QString::fromLatin1(name);
        }
    }

    void staleAcknowledgementCannotRestoreCurrentSession()
    {
        QFETCH(QString, change);
        Fixture fixture;
        fixture.hold = true;
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost, 0));
        QTemporaryDir directory;
        UomReportingService service(directory.filePath(QStringLiteral("reports.sqlite")));
        configure(service, fixture.profile());
        QTRY_VERIFY_WITH_TIMEOUT(service.ready(), TEST_TIMEOUT_MS);
        auto capture = fixture.capture();
        service.capture(capture);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.bodies.size(), 1, TEST_TIMEOUT_MS);
        capture.sample.timestampMs += 1;
        if (change == QStringLiteral("boot")) {
            capture.bootId += 1;
        } else if (change == QStringLiteral("identity")) {
            capture.identity["revision"] = 2;
        } else if (change == QStringLiteral("connection")) {
            capture.connection += 1;
        } else if (change == QStringLiteral("flight")) {
            capture.flightSequence += 1;
            capture.takeoffUtcMs = capture.sample.timestampMs;
        } else if (change == QStringLiteral("model")) {
            capture.profile.uasModel = QStringLiteral("OTHER-MODEL");
        } else if (change == QStringLiteral("timestamp")) {
            capture.sample.timestampMs += 5000;
        } else {
            capture.airborne = false;
        }
        service.capture(capture);
        QTRY_COMPARE_WITH_TIMEOUT(service.status(AIRCRAFT).value("total").toInt(), 2, TEST_TIMEOUT_MS);
        fixture.respond(0);
        QTRY_COMPARE_WITH_TIMEOUT(service.status(AIRCRAFT).value("accepted").toInt(), 1, TEST_TIMEOUT_MS);
        QVERIFY(!service.status(AIRCRAFT).value("healthy").toBool());
    }

    void authenticationFailurePausesSendingButKeepsRecording()
    {
        Fixture fixture;
        fixture.code = 401;
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost, 0));
        QTemporaryDir directory;
        UomReportingService service(directory.filePath(QStringLiteral("reports.sqlite")));
        configure(service, fixture.profile());
        QTRY_VERIFY_WITH_TIMEOUT(service.ready(), TEST_TIMEOUT_MS);
        auto capture = fixture.capture();
        service.capture(capture);
        QTRY_VERIFY_WITH_TIMEOUT(service.status(AIRCRAFT).value("rejected").toBool(), TEST_TIMEOUT_MS);
        QVERIFY(!service.status(AIRCRAFT).value("healthy").toBool());
        capture.sample.timestampMs += 1000;
        service.capture(capture);
        QTRY_COMPARE_WITH_TIMEOUT(service.status(AIRCRAFT).value("total").toInt(), 2, TEST_TIMEOUT_MS);
        QCOMPARE(fixture.bodies.size(), 1);
        QCOMPARE(service.status(AIRCRAFT).value("pending").toInt(), 2);
        QVERIFY(service.hasOutstandingWork());
        QVERIFY(service.ready());
    }

    void uncertainHistoryStaysUnsentUntilBackfillIsValidated()
    {
        Fixture fixture;
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost, 0));
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("reports.sqlite"));
        auto capture = fixture.capture();
        capture.sample.timestampMs -= 10000;
        capture.takeoffUtcMs -= 10000;
        QByteArray oldBody;
        {
            UomReportStore store(path);
            QVERIFY(store.append(capture, capture.sample.timestampMs));
            const auto due = store.pending(capture.sample.timestampMs);
            QCOMPARE(due.size(), 1);
            oldBody = due[0].body;
            QVERIFY(store.reserve(due[0].id, capture.sample.timestampMs));
        }
        UomReportingService service(path);
        configure(service, fixture.profile());
        QTRY_VERIFY_WITH_TIMEOUT(service.ready(), TEST_TIMEOUT_MS);
        capture.sample.timestampMs += 10000;
        service.capture(capture);
        QTRY_VERIFY_WITH_TIMEOUT(service.status(AIRCRAFT).value("healthy").toBool(), TEST_TIMEOUT_MS);
        QCOMPARE(fixture.bodies.size(), 1);
        QVERIFY(fixture.bodies[0] != oldBody);
        QCOMPARE(service.status(AIRCRAFT).value("pending").toInt(), 1);
    }

    void backlogIsFairAndAuthenticationPauseIsScopedToAccount()
    {
        Fixture accountA;
        Fixture accountB;
        accountA.hold = true;
        QVERIFY(accountA.server.listen(QHostAddress::LocalHost, 0));
        QVERIFY(accountB.server.listen(QHostAddress::LocalHost, 0));
        auto profileA = accountA.profile();
        profileA.appId = QStringLiteral("account-a");
        profileA.credentialReference = QStringLiteral("uom-reporting/account-a");
        profileA.backfillValidated = true;
        auto profileB = accountB.profile();
        profileB.appId = QStringLiteral("account-b");
        profileB.credentialReference = QStringLiteral("uom-reporting/account-b");
        const QString otherAircraftA = QStringLiteral("1581F45T0123456789AC");
        const QString aircraftB = QStringLiteral("1581F45T0123456789AD");
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("reports.sqlite"));
        // Exceed the scheduling candidate window while a historical HTTP request remains in flight.
        constexpr int backlogSize = 256;
        {
            UomReportStore store(path);
            auto historical = accountA.capture();
            historical.profile = profileA;
            historical.sample.timestampMs -= 10000 + backlogSize * 500;
            historical.takeoffUtcMs = historical.sample.timestampMs - 1000;
            for (int i = 0; i < backlogSize; ++i) {
                QVERIFY(store.append(historical, historical.sample.timestampMs));
                historical.sample.timestampMs += 500;
            }
        }
        UomReportingService service(path);
        const UomReportingService::Binding bindingA{.profile = profileA, .aircraftIds = {AIRCRAFT, otherAircraftA}};
        const UomReportingService::Binding bindingB{.profile = profileB, .aircraftIds = {aircraftB}};
        service._configureIsolated({bindingA, bindingB}, {{profileA.credentialReference, QStringLiteral("key-a")},
                                                          {profileB.credentialReference, QStringLiteral("key-b")}});
        QTRY_COMPARE_WITH_TIMEOUT(accountA.bodies.size(), 1, TEST_TIMEOUT_MS);
        auto liveA = accountA.capture();
        liveA.profile = profileA;
        liveA.sample.aircraftId = otherAircraftA;
        auto liveB = accountB.capture();
        liveB.profile = profileB;
        liveB.sample.aircraftId = aircraftB;
        service.capture(liveA);
        service.capture(liveB);
        QTRY_COMPARE_WITH_TIMEOUT(accountA.bodies.size(), 2, TEST_TIMEOUT_MS);
        QTRY_VERIFY_WITH_TIMEOUT(service.status(aircraftB).value("healthy").toBool(), TEST_TIMEOUT_MS);
        QCOMPARE(accountA.bodies[1], encode(liveA.sample)->toHex().toUpper());
        QCOMPARE(service.status(AIRCRAFT).value("pending").toInt(), backlogSize);
        accountA.respond(1);
        QTRY_VERIFY_WITH_TIMEOUT(service.status(otherAircraftA).value("healthy").toBool(), TEST_TIMEOUT_MS);

        accountA.code = 401;
        accountA.respond(0);
        QTRY_COMPARE_WITH_TIMEOUT(service._events.value("authentication").toInt(), 1, TEST_TIMEOUT_MS);
        liveA.sample.timestampMs = qMax(liveA.sample.timestampMs + 1, quint64(QDateTime::currentMSecsSinceEpoch()));
        liveB.sample.timestampMs = qMax(liveB.sample.timestampMs + 1, quint64(QDateTime::currentMSecsSinceEpoch()));
        service.capture(liveA);
        service.capture(liveB);
        QTRY_COMPARE_WITH_TIMEOUT(service.status(aircraftB).value("accepted").toInt(), 2, TEST_TIMEOUT_MS);
        QTRY_COMPARE_WITH_TIMEOUT(service.status(otherAircraftA).value("pending").toInt(), 1, TEST_TIMEOUT_MS);
        QCOMPARE(service.status(otherAircraftA).value("accepted").toInt(), 1);
        QCOMPARE(accountA.bodies.size(), 2);
        QCOMPARE(accountB.bodies.size(), 2);
        QVERIFY(!service.status(otherAircraftA).value("healthy").toBool());
        QVERIFY(service.status(aircraftB).value("healthy").toBool());
    }

    void missingCredentialKeepsSamplesUntilValidatedBackfill()
    {
        Fixture fixture;
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost, 0));
        QTemporaryDir directory;
        UomReportingService service(directory.filePath(QStringLiteral("reports.sqlite")));
        auto profile = fixture.profile();
        const UomReportingService::Binding binding{.profile = profile, .aircraftIds = {AIRCRAFT}};
        service._configureIsolated({binding}, {});
        QTRY_VERIFY_WITH_TIMEOUT(service.ready(), TEST_TIMEOUT_MS);
        QVERIFY(!service.credentialAvailable(profile.credentialReference));
        const auto capture = fixture.capture();
        service.capture(capture);
        QTRY_COMPARE_WITH_TIMEOUT(service.status(AIRCRAFT).value("pending").toInt(), 1, TEST_TIMEOUT_MS);
        QVERIFY(service.hasOutstandingWork());
        QVERIFY(fixture.bodies.isEmpty());
        service.disconnectAircraft(AIRCRAFT, capture.connection);
        configure(service, profile);
        QTRY_COMPARE_WITH_TIMEOUT(service._events.value("disconnected").toInt(), 1, TEST_TIMEOUT_MS);
        QVERIFY(service.credentialAvailable(profile.credentialReference));
        QVERIFY(fixture.bodies.isEmpty());
        QCOMPARE(service.status(AIRCRAFT).value("pending").toInt(), 1);
        profile.backfillValidated = true;
        configure(service, profile);
        QTRY_COMPARE_WITH_TIMEOUT(service.status(AIRCRAFT).value("accepted").toInt(), 1, TEST_TIMEOUT_MS);
        QCOMPARE(fixture.bodies.size(), 1);
        QCOMPARE(fixture.bodies[0], encode(capture.sample)->toHex().toUpper());
        QVERIFY(!service.status(AIRCRAFT).value("healthy").toBool());
    }

    void profileSwitchDoesNotStarveAuthorizedBackfill_data()
    {
        QTest::addColumn<bool>("changeAccount");
        QTest::newRow("account") << true;
        QTest::newRow("model") << false;
    }

    void profileSwitchDoesNotStarveAuthorizedBackfill()
    {
        QFETCH(bool, changeAccount);
        Fixture fixture;
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost, 0));
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("reports.sqlite"));
        auto historical = fixture.capture();
        constexpr int backlogSize = 256;
        historical.sample.timestampMs -= 10000 + backlogSize * 500;
        historical.takeoffUtcMs = historical.sample.timestampMs - 1000;
        {
            UomReportStore store(path);
            for (int i = 0; i < backlogSize; ++i) {
                QVERIFY(store.append(historical, historical.sample.timestampMs).value_or(0) > 0);
                historical.sample.timestampMs += 500;
            }
            if (changeAccount) {
                historical.profile.appId = QStringLiteral("new-account");
            } else {
                historical.profile.uasModel = QStringLiteral("NEW-MODEL");
            }
            ++historical.flightSequence;
            historical.takeoffUtcMs = historical.sample.timestampMs - 1000;
            QVERIFY(store.append(historical, historical.sample.timestampMs).value_or(0) > 0);
        }
        historical.profile.backfillValidated = true;
        UomReportingService service(path);
        configure(service, historical.profile);
        QTRY_COMPARE_WITH_TIMEOUT(service.status(AIRCRAFT).value("accepted").toInt(), 1, TEST_TIMEOUT_MS);
        QCOMPARE(fixture.bodies.size(), 1);
        QCOMPARE(fixture.bodies[0], encode(historical.sample)->toHex().toUpper());
        QCOMPARE(service.status(AIRCRAFT).value("pending").toInt(), backlogSize);
        QCOMPARE(service.status(AIRCRAFT).value("total").toInt(), backlogSize + 1);
        QVERIFY(!service.status(AIRCRAFT).value("healthy").toBool());
    }

    void reconnectDoesNotClaimPreviouslyQueuedSample()
    {
        Fixture fixture;
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost, 0));
        QTemporaryDir directory;
        UomReportingService service(directory.filePath(QStringLiteral("reports.sqlite")));
        const auto profile = fixture.profile();
        const UomReportingService::Binding binding{.profile = profile, .aircraftIds = {AIRCRAFT}};
        service._configureIsolated({binding}, {});
        QTRY_VERIFY_WITH_TIMEOUT(service.ready(), TEST_TIMEOUT_MS);
        auto capture = fixture.capture();
        service.capture(capture);
        ++capture.connection;
        ++capture.sample.timestampMs;
        service.capture(capture);
        QTRY_COMPARE_WITH_TIMEOUT(service.status(AIRCRAFT).value("pending").toInt(), 2, TEST_TIMEOUT_MS);
        configure(service, profile);
        QTRY_COMPARE_WITH_TIMEOUT(service.status(AIRCRAFT).value("accepted").toInt(), 1, TEST_TIMEOUT_MS);
        QCOMPARE(service.status(AIRCRAFT).value("pending").toInt(), 1);
        QCOMPARE(fixture.bodies.size(), 1);
        QCOMPARE(fixture.bodies[0], encode(capture.sample)->toHex().toUpper());
        QVERIFY(service.status(AIRCRAFT).value("healthy").toBool());
    }

    void completedFlightsDoNotBecomeNetworkTimeouts()
    {
        Fixture fixture;
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost, 0));
        QTemporaryDir directory;
        UomReportingService ground(directory.filePath(QStringLiteral("ground.sqlite")));
        UomReportingService disconnected(directory.filePath(QStringLiteral("disconnected.sqlite")));
        configure(ground, fixture.profile());
        configure(disconnected, fixture.profile());
        QTRY_VERIFY_WITH_TIMEOUT(ground.ready() && disconnected.ready(), TEST_TIMEOUT_MS);
        auto capture = fixture.capture();
        ground.capture(capture);
        disconnected.capture(capture);
        QTRY_VERIFY_WITH_TIMEOUT(ground.status(AIRCRAFT).value("healthy").toBool() &&
                                     disconnected.status(AIRCRAFT).value("healthy").toBool(),
                                 TEST_TIMEOUT_MS);
        disconnected.disconnectAircraft(AIRCRAFT, capture.connection);
        capture.airborne = false;
        ++capture.sample.timestampMs;
        ground.capture(capture);
        QTRY_VERIFY_WITH_TIMEOUT(ground.status(AIRCRAFT).value("ackAgeMs").toLongLong() > 5000 &&
                                     disconnected.status(AIRCRAFT).value("ackAgeMs").toLongLong() > 5000,
                                 TEST_TIMEOUT_MS + 1000);
        QVERIFY(!ground.status(AIRCRAFT).value("networkFault").toBool());
        QVERIFY(!disconnected.status(AIRCRAFT).value("networkFault").toBool());
        QVERIFY(!ground.status(AIRCRAFT).value("healthy").toBool());
        QVERIFY(!disconnected.status(AIRCRAFT).value("healthy").toBool());
    }

    void workerQueueDelayDoesNotRenewSampleFreshness()
    {
        Fixture fixture;
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost, 0));
        QTemporaryDir directory;
        UomReportingService service(directory.filePath(QStringLiteral("reports.sqlite")));
        configure(service, fixture.profile());
        QTRY_VERIFY_WITH_TIMEOUT(service.ready(), TEST_TIMEOUT_MS);
        const auto entered = std::make_shared<QSemaphore>();
        const auto release = std::make_shared<QSemaphore>();
        auto* barrier = new QObject;
        barrier->moveToThread(&service._thread);
        QMetaObject::invokeMethod(barrier, [barrier, entered, release]() {
            entered->release();
            (void) release->tryAcquire(1, TEST_TIMEOUT_MS);
            barrier->deleteLater();
        });
        QTRY_COMPARE_WITH_TIMEOUT(entered->available(), 1, TEST_TIMEOUT_MS);
        QSignalSpy changes(&service, &UomReportingService::changed);
        auto capture = fixture.capture();
        service.capture(capture);
        QTimer unblock;
        unblock.setSingleShot(true);
        unblock.setTimerType(Qt::PreciseTimer);
        connect(&unblock, &QTimer::timeout, &service, [release]() { release->release(); });
        unblock.start(1100);
        QTRY_COMPARE_WITH_TIMEOUT(service._queuedCaptures, 0, TEST_TIMEOUT_MS);
        const auto updatesBeforeTick = changes.count();
        QTRY_VERIFY_WITH_TIMEOUT(changes.count() > updatesBeforeTick, TEST_TIMEOUT_MS);
        QCOMPARE(service.status(AIRCRAFT).value("pending").toInt(), 1);
        QVERIFY(!service.status(AIRCRAFT).value("healthy").toBool());
        QVERIFY(fixture.bodies.isEmpty());

        capture.sample.timestampMs = quint64(QDateTime::currentMSecsSinceEpoch());
        service.capture(capture);
        QTRY_VERIFY_WITH_TIMEOUT(service.status(AIRCRAFT).value("healthy").toBool(), TEST_TIMEOUT_MS);
        QCOMPARE(service.status(AIRCRAFT).value("pending").toInt(), 1);
        QCOMPARE(service.status(AIRCRAFT).value("accepted").toInt(), 1);
        QCOMPARE(fixture.bodies.size(), 1);
        QCOMPARE(fixture.bodies[0], encode(capture.sample)->toHex().toUpper());
    }

    void boundedCaptureQueueRecordsARecoverableGap()
    {
        Fixture fixture;
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost, 0));
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("reports.sqlite"));
        UomReportingService service(path);
        configure(service, fixture.profile());
        QTRY_VERIFY_WITH_TIMEOUT(service.ready(), TEST_TIMEOUT_MS);
        auto capture = fixture.capture();
        capture.airborne = false;
        capture.provenance = {{"gpsUtcUs", QString::number(capture.sample.timestampMs * 1000)},
                              {"altitudeDatum", "WGS84"}};
        for (int i = 0; i < 65; ++i) {
            ++capture.sample.timestampMs;
            service.capture(capture);
        }
        QCOMPARE(service._queuedCaptures, 64);
        QCOMPARE(service._droppedCaptures, 1);
        QVERIFY(!service.ready());
        QTRY_COMPARE_WITH_TIMEOUT(service._queuedCaptures, 0, TEST_TIMEOUT_MS);
        QVERIFY(service._captureFault);
        ++capture.sample.timestampMs;
        service.capture(capture);
        QTRY_VERIFY_WITH_TIMEOUT(service.ready(), TEST_TIMEOUT_MS);
        QVERIFY(!service._captureFault);
        QCOMPARE(service._droppedCaptures, 1);
        service.shutdown();
        const QString name = QStringLiteral("gap-verification");
        {
            auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            database.setDatabaseName(path);
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec(QStringLiteral("SELECT sample,source_ms FROM samples ORDER BY id DESC LIMIT 1")));
            QVERIFY(query.next());
            const auto json = QJsonDocument::fromJson(query.value(0).toString().toUtf8()).object();
            const auto metadata = json.value("sourceMetadata").toObject();
            QCOMPARE(metadata.value("droppedCapturesBefore").toString(), QStringLiteral("1"));
            QCOMPARE(metadata.value("altitudeDatum").toString(), QStringLiteral("WGS84"));
            QCOMPARE(query.value(1).toULongLong(), capture.sample.timestampMs);
            query.finish();
            database.close();
        }
        QSqlDatabase::removeDatabase(name);
        QVERIFY(fixture.bodies.isEmpty());
    }

    void staleSourceDataProducesEventsWithoutForgedSamples()
    {
        Fixture fixture;
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost, 0));
        QTemporaryDir directory;
        UomReportingService service(directory.filePath(QStringLiteral("reports.sqlite")));
        configure(service, fixture.profile());
        QTRY_VERIFY_WITH_TIMEOUT(service.ready(), TEST_TIMEOUT_MS);
        auto capture = fixture.capture();
        capture.airborne = false;
        service.capture(capture);
        QTRY_COMPARE_WITH_TIMEOUT(service.status(AIRCRAFT).value("total").toInt(), 1, TEST_TIMEOUT_MS);
        capture.valid = false;
        capture.issue = QStringLiteral("GPS source timestamp stale");
        service.capture(capture);
        QTRY_COMPARE_WITH_TIMEOUT(service._events.value("invalid-data").toInt(), 1, TEST_TIMEOUT_MS);
        QCOMPARE(service.status(AIRCRAFT).value("total").toInt(), 1);
        service.disconnectAircraft(AIRCRAFT, capture.connection);
        QTRY_COMPARE_WITH_TIMEOUT(service._events.value("disconnected").toInt(), 1, TEST_TIMEOUT_MS);
        const QString diagnosticPath = directory.filePath(QStringLiteral("diagnostics.json"));
        QVERIFY(service.exportDiagnostics(diagnosticPath));
        QFile diagnostic(diagnosticPath);
        QVERIFY(diagnostic.open(QIODevice::ReadOnly));
        const auto bytes = diagnostic.readAll();
        QVERIFY(!bytes.contains(AIRCRAFT.toUtf8()));
        QVERIFY(!bytes.contains("GPS source timestamp stale"));
        QVERIFY(!bytes.contains("test-key"));
        QCOMPARE(QJsonDocument::fromJson(bytes).object().value("events").toObject().value("disconnected").toInt(), 1);
        QVERIFY(fixture.bodies.isEmpty());
    }

    void restartedQueueIsVisibleBeforeAnyVehicleCapture()
    {
        Fixture fixture;
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost, 0));
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("reports.sqlite"));
        auto capture = fixture.capture();
        {
            UomReportStore store(path);
            QVERIFY(store.append(capture, capture.sample.timestampMs));
            capture.airborne = false;
            ++capture.sample.timestampMs;
            QVERIFY(store.append(capture, capture.sample.timestampMs));
        }
        UomReportingService service(path);
        configure(service, fixture.profile());
        QTRY_COMPARE_WITH_TIMEOUT(service.status(AIRCRAFT).value("total").toInt(), 2, TEST_TIMEOUT_MS);
        QCOMPARE(service.status(AIRCRAFT).value("pending").toInt(), 1);
        QCOMPARE(service.status(AIRCRAFT).value("held").toInt(), 1);
        QVERIFY(!service.status(AIRCRAFT).value("healthy").toBool());
        QVERIFY(!service.status(AIRCRAFT).value("started").toBool());
        QVERIFY(service.hasOutstandingWork());
        QVERIFY(fixture.bodies.isEmpty());
    }

    void invalidStorageAndGroundNeverClaimHealthy()
    {
        Fixture fixture;
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost, 0));
        QTemporaryDir directory;
        UomReportingService groundService(directory.filePath(QStringLiteral("ground.sqlite")));
        configure(groundService, fixture.profile());
        QTRY_VERIFY_WITH_TIMEOUT(groundService.ready(), TEST_TIMEOUT_MS);
        auto capture = fixture.capture();
        capture.airborne = false;
        groundService.capture(capture);
        QTRY_COMPARE_WITH_TIMEOUT(groundService.status(AIRCRAFT).value("held").toInt(), 1, TEST_TIMEOUT_MS);
        QVERIFY(fixture.bodies.isEmpty());
        QVERIFY(!groundService.status(AIRCRAFT).value("healthy").toBool());
        QFile file(directory.filePath(QStringLiteral("not-directory")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("blocked");
        file.close();
        UomReportingService failed(file.fileName() + QStringLiteral("/reports.sqlite"));
        QSignalSpy spy(&failed, &UomReportingService::changed);
        QTRY_VERIFY_WITH_TIMEOUT(!spy.isEmpty(), TEST_TIMEOUT_MS);
        QVERIFY(!failed.ready());
    }
};

QTEST_GUILESS_MAIN(UomReportingServiceTest)
#include "UomReportingServiceTest.moc"
