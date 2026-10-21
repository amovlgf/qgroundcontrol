#include <QtCore/QJsonDocument>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QScopeGuard>
#include <QtCore/QTimer>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include "MockUomProvider.h"
#include "UomSession.h"

class HttpFixture : public QObject
{
public:
    HttpFixture()
    {
        connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (auto* socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer = QByteArray{}]() mutable {
                    buffer += socket->readAll();
                    const auto boundary = buffer.indexOf("\r\n\r\n");
                    if (boundary < 0) {
                        return;
                    }
                    int size = -1;
                    for (const auto& line : buffer.left(boundary).split('\n')) {
                        if (line.toLower().startsWith("content-length:")) {
                            size = line.mid(15).trimmed().toInt();
                        }
                    }
                    if (size < 0 || buffer.size() < boundary + 4 + size) {
                        return;
                    }
                    const auto request = QJsonDocument::fromJson(buffer.mid(boundary + 4, size)).object();
                    lastRequest = request;
                    if (mode == "hold") {
                        return;
                    }
                    authorizationSeen = buffer.toLower().contains("authorization: bearer test-token");
                    auto response =
                        QJsonObject{{"protocol", "uom-test-v1"},
                                    {"environment", "mock"},
                                    {"generation", request.value("generation")},
                                    {"sequence", request.value("sequence")},
                                    {"identity_digest", request.value("identity").toObject().value("digest")},
                                    {"accepted", true},
                                    {"token", "TEST-TOKEN"}};
                    if (mode == "wrong-identity") {
                        response["identity_digest"] = "wrong";
                    }
                    if (mode == "wrong-environment") {
                        response["environment"] = "official";
                    }
                    if (mode == "wrong-sequence") {
                        response["sequence"] = "99";
                    }
                    if (mode == "reject") {
                        response["accepted"] = false;
                    }
                    QByteArray body = mode == "malformed" ? QByteArray("{no-json") : QJsonDocument(response).toJson();
                    const QByteArray status = mode == "unauthorized" ? "401 Unauthorized" : "200 OK";
                    socket->write("HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nContent-Length: " +
                                  QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
    }

    QTcpServer server;
    QString mode;
    QJsonObject lastRequest;
    bool authorizationSeen = false;
};

class HttpUomTest : public QObject
{
    Q_OBJECT
private slots:

    void realPythonServiceRoundTripAndRecovery()
    {
        QProcess server;
        const auto stop = qScopeGuard([&]() {
            server.terminate();
            if (!server.waitForFinished(3000)) {
                server.kill();
                server.waitForFinished(3000);
            }
        });
        server.start("python3", {QStringLiteral(UOM_MOCK_SERVER), "--port", "0"});
        QVERIFY(server.waitForStarted(5000));
        QVERIFY(server.waitForReadyRead(5000));
        const auto output = QString::fromUtf8(server.readAllStandardOutput());
        const auto match = QRegularExpression("http://127\\.0\\.0\\.1:[0-9]+").match(output);
        QVERIFY2(match.hasMatch(), qPrintable(output));
        const QUrl endpoint(match.captured());
        MockUomProvider provider(endpoint);
        UomSession session(&provider);
        Uom::Identity id;
        id.aircraftId = "TESTAIRCRAFT00000001";
        id.moduleId = "001122AABBCC";
        id.bootId = 1;
        id.ridBootId = 2;
        id.revision = 3;
        id.activated = true;
        id.digest = id.computedDigest();
        session.setIdentity(id);
        QTimer dataTimer;
        const auto update = [&]() {
            Uom::Telemetry data;
            data.receivedUs = session.nowUs();
            data.utcMilliseconds = 1800000000000;
            data.positionValid = data.operatorValid = true;
            session.setTelemetry(data);
        };
        connect(&dataTimer, &QTimer::timeout, &session, update);
        dataTimer.start(200);
        update();
        QTRY_VERIFY_WITH_TIMEOUT(session.simulationReady(), 5000);
        QVERIFY(!session.officialReady());

        QNetworkAccessManager controller;
        controller.setProxy(QNetworkProxy::NoProxy);
        QUrl faultUrl(endpoint);
        faultUrl.setPath("/control/fault");
        QNetworkRequest request(faultUrl);
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        auto* reply = controller.post(request, R"({"fault":"unauthorized","phase":"report","count":1})");
        QSignalSpy completed(reply, &QNetworkReply::finished);
        QVERIFY(completed.wait(2000));
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
        reply->deleteLater();
        QTRY_COMPARE_WITH_TIMEOUT(session.fault(), Uom::Fault::Authentication, 3000);
        QVERIFY(!session.simulationReady());
        QTRY_VERIFY_WITH_TIMEOUT(session.simulationReady(), 5000);
        QVERIFY(!session.officialReady());
    }

    void roundTrip()
    {
        HttpFixture fixture;
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost, 0));
        MockUomProvider provider(QUrl(QString("http://127.0.0.1:%1").arg(fixture.server.serverPort())));
        QSignalSpy auth(&provider, &UomProvider::authenticated);
        QSignalSpy report(&provider, &UomProvider::acknowledged);
        Uom::Identity id;
        id.digest = QByteArray(32, 'A');
        provider.authenticate(7, id);
        QVERIFY(auth.wait(2000));
        QCOMPARE(auth.at(0).at(0).toULongLong(), quint64(7));
        QVERIFY(auth.at(0).at(1).toBool());
        provider.submit(7, 3, id, {});
        QVERIFY(report.wait(2000));
        QCOMPARE(report.at(0).at(1).toULongLong(), quint64(3));
        QVERIFY(report.at(0).at(2).toBool());
        QVERIFY(fixture.authorizationSeen);
        QCOMPARE(fixture.lastRequest.value("environment").toString(), QString("mock"));
    }

    void faults_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto* mode :
             {"unauthorized", "malformed", "wrong-identity", "wrong-environment", "wrong-sequence", "reject"}) {
            QTest::newRow(mode) << QString(mode);
        }
    }

    void faults()
    {
        QFETCH(QString, mode);
        HttpFixture fixture;
        fixture.mode = mode;
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost, 0));
        MockUomProvider provider(QUrl(QString("http://127.0.0.1:%1").arg(fixture.server.serverPort())));
        QSignalSpy auth(&provider, &UomProvider::authenticated);
        Uom::Identity id;
        id.digest = QByteArray(32, 'A');
        provider.authenticate(9, id);
        QVERIFY(auth.wait(2000));
        QVERIFY(!auth.at(0).at(1).toBool());
        fixture.mode.clear();
        provider.authenticate(10, id);
        QVERIFY(auth.wait(2000));
        QCOMPARE(auth.at(1).at(0).toULongLong(), quint64(10));
        QVERIFY(auth.at(1).at(1).toBool());
    }

    void ignoresApplicationProxy()
    {
        const auto previous = QNetworkProxy::applicationProxy();
        const auto restore = qScopeGuard([&]() { QNetworkProxy::setApplicationProxy(previous); });
        QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1", 1));
        HttpFixture fixture;
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost, 0));
        MockUomProvider provider(QUrl(QString("http://127.0.0.1:%1").arg(fixture.server.serverPort())));
        QSignalSpy auth(&provider, &UomProvider::authenticated);
        Uom::Identity id;
        id.digest = QByteArray(32, 'A');
        provider.authenticate(1, id);
        QVERIFY(auth.wait(2000));
        QVERIFY(auth.at(0).at(1).toBool());
    }

    void cancelSuppressesOldReply()
    {
        HttpFixture fixture;
        fixture.mode = "hold";
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost, 0));
        MockUomProvider provider(QUrl(QString("http://127.0.0.1:%1").arg(fixture.server.serverPort())));
        QSignalSpy auth(&provider, &UomProvider::authenticated);
        Uom::Identity id;
        id.digest = QByteArray(32, 'A');
        provider.authenticate(1, id);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.lastRequest.isEmpty(), 2000);
        provider.cancel();
        fixture.mode.clear();
        provider.authenticate(2, id);
        QVERIFY(auth.wait(2000));
        QCOMPARE(auth.size(), 1);
        QCOMPARE(auth.at(0).at(0).toULongLong(), quint64(2));
        QVERIFY(auth.at(0).at(1).toBool());
    }

    void offline()
    {
        QTcpServer portReservation;
        QVERIFY(portReservation.listen(QHostAddress::LocalHost, 0));
        const auto port = portReservation.serverPort();
        portReservation.close();
        MockUomProvider provider(QUrl(QString("http://127.0.0.1:%1").arg(port)));
        QSignalSpy auth(&provider, &UomProvider::authenticated);
        provider.authenticate(1, {});
        QVERIFY(auth.wait(3000));
        QVERIFY(!auth.at(0).at(1).toBool());
    }
};
QTEST_GUILESS_MAIN(HttpUomTest)
#include "HttpUomTest.moc"
