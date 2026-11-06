#include <QtCore/QTimer>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include "UomReportClient.h"

using namespace UomReporting;

namespace {
// These standalone tests do not link QGC's application harness. Bound all local socket waits to five seconds.
constexpr int TEST_TIMEOUT_MS = 5000;
const QByteArray BODY =
    "FF2048FFFFFE31353831463435543031323334353637383941423132333435363738010100A0A2CF44D8F3F417B009"
    "A0A2CF44D8F3F417080A1900544716280A140A010001010108573BD79B0101";
const QString ORDER_ID = QStringLiteral("20260120-AbCd1234");

class HttpFixture : public QObject
{
public:
    HttpFixture()
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
                    int size = -1;
                    for (const auto& line : buffer.left(boundary).split('\n')) {
                        if (line.toLower().startsWith("content-length:")) {
                            size = line.mid(15).trimmed().toInt();
                        }
                    }
                    if (size < 0 || buffer.size() < boundary + 4 + size) {
                        return;
                    }
                    socket->setProperty("handled", true);
                    requests.append(buffer.left(boundary));
                    bodies.append(buffer.mid(boundary + 4, size));
                    if (hold) {
                        return;
                    }
                    socket->write("HTTP/1.1 " + QByteArray::number(status) +
                                  " Test\r\nContent-Type: application/json\r\n"
                                  "Content-Length: " +
                                  QByteArray::number(response.size()) +
                                  "\r\nLocation: /must-not-follow\r\nConnection: close\r\n\r\n" + response);
                    socket->disconnectFromHost();
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
                .platform = QStringLiteral("QGC-offline"),
                .credentialReference = QStringLiteral("offline-key")};
    }

    QTcpServer server;
    QByteArray response = R"({"code":200,"msg":"success"})";
    QList<QByteArray> requests;
    QList<QByteArray> bodies;
    int status = 200;
    bool hold = false;
};
}  // namespace

class UomReportClientTest : public QObject
{
    Q_OBJECT
private slots:

    void roundTripAndConcurrentCorrelation()
    {
        HttpFixture fixture;
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost, 0));
        UomReportClient client;
        QSignalSpy spy(&client, &UomReportClient::completed);
        for (quint64 id : {42, 7, 99}) {
            client.send(id, fixture.profile(), QStringLiteral("test-app-key"), ORDER_ID, BODY);
        }
        QCOMPARE(client.pendingCount(), 3);
        QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 3, TEST_TIMEOUT_MS);
        QCOMPARE(fixture.requests.size(), 3);
        QSet<quint64> received;
        for (const auto& arguments : spy) {
            received.insert(arguments[0].toULongLong());
            QVERIFY(qvariant_cast<Result>(arguments[1]).success);
        }
        QCOMPARE(received, (QSet<quint64>{42, 7, 99}));
        QVERIFY(!client.busy());
        for (qsizetype i = 0; i < fixture.requests.size(); ++i) {
            const auto headers = fixture.requests[i].toLower();
            QVERIFY(headers.startsWith("post /riddata http/1.1"));
            QVERIFY(headers.contains("content-type: application/octet-stream"));
            QVERIFY(headers.contains("appid: offline-app"));
            QVERIFY(headers.contains("source: 1"));
            QVERIFY(headers.contains("orderid: 20260120-abcd1234"));
            QVERIFY(headers.contains("sign: 294c1a1ec137b651ec704642283f462cea5156a9cc62cfc9788ed943f76500fa"));
            QVERIFY(!headers.contains("test-app-key"));
            QCOMPARE(fixture.bodies[i], BODY);
        }
    }

    void rejectsTransportAndBusinessFailures_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<bool>("retryable");
        QTest::newRow("redirect") << 302 << QByteArray(R"({"code":200,"msg":"success"})") << false;
        QTest::newRow("business400") << 200 << QByteArray(R"({"code":400,"msg":"bad"})") << false;
        QTest::newRow("business401") << 200 << QByteArray(R"({"code":401,"msg":"bad"})") << false;
        QTest::newRow("http500") << 500 << QByteArray(R"({"code":500,"msg":"bad"})") << true;
        QTest::newRow("malformed") << 200 << QByteArray("<html>bad</html>") << false;
        QTest::newRow("oversized") << 200 << QByteArray(MAX_RESPONSE_BYTES + 1, 'x') << false;
        QTest::newRow("wrong-http-success") << 201 << QByteArray(R"({"code":200,"msg":"success"})") << false;
    }

    void rejectsTransportAndBusinessFailures()
    {
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        QFETCH(bool, retryable);
        HttpFixture fixture;
        fixture.status = status;
        fixture.response = body;
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost, 0));
        UomReportClient client;
        QSignalSpy spy(&client, &UomReportClient::completed);
        client.send(1, fixture.profile(), QStringLiteral("test-key"), ORDER_ID, BODY);
        QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, TEST_TIMEOUT_MS);
        const auto result = qvariant_cast<Result>(spy[0][1]);
        QVERIFY(!result.success);
        QCOMPARE(result.retryable, retryable);
        QCOMPARE(fixture.requests.size(), 1);
        QVERIFY(!client.busy());
    }

    void cancellationSuppressesStaleRepliesAndAllowsReusedId()
    {
        HttpFixture fixture;
        fixture.hold = true;
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost, 0));
        UomReportClient client;
        QSignalSpy spy(&client, &UomReportClient::completed);
        client.send(1, fixture.profile(), QStringLiteral("test-key"), ORDER_ID, BODY);
        client.send(2, fixture.profile(), QStringLiteral("test-key"), ORDER_ID, BODY);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.requests.size(), 2, TEST_TIMEOUT_MS);
        client.cancel(1);
        QCOMPARE(client.pendingCount(), 1);
        client.cancel();
        QVERIFY(!client.busy());
        fixture.hold = false;
        client.send(1, fixture.profile(), QStringLiteral("test-key"), ORDER_ID, BODY);
        QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, TEST_TIMEOUT_MS);
        QCOMPARE(spy[0][0].toULongLong(), quint64(1));
        QVERIFY(qvariant_cast<Result>(spy[0][1]).success);
        QCOMPARE(fixture.requests.size(), 3);
    }

    void absoluteTimeoutIsRetryable()
    {
        HttpFixture fixture;
        fixture.hold = true;
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost, 0));
        QNetworkAccessManager network;
        network.setProxy(QNetworkProxy::NoProxy);
        // The short injected deadline exercises expiry without a five-second test delay.
        UomReportClient client(&network, nullptr, 100);
        QSignalSpy spy(&client, &UomReportClient::completed);
        client.send(9, fixture.profile(), QStringLiteral("test-key"), ORDER_ID, BODY);
        QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, TEST_TIMEOUT_MS);
        const auto result = qvariant_cast<Result>(spy[0][1]);
        QVERIFY(!result.success);
        QVERIFY(result.retryable);
        QVERIFY(!client.busy());
    }

    void capacityAndDuplicateIds()
    {
        HttpFixture fixture;
        fixture.hold = true;
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost, 0));
        UomReportClient client;
        QSignalSpy spy(&client, &UomReportClient::completed);
        for (quint64 id = 1; id <= 32; ++id) {
            client.send(id, fixture.profile(), QStringLiteral("test-key"), ORDER_ID, BODY);
        }
        QCOMPARE(client.pendingCount(), 32);
        client.send(1, fixture.profile(), QStringLiteral("test-key"), ORDER_ID, BODY);
        client.send(33, fixture.profile(), QStringLiteral("test-key"), ORDER_ID, BODY);
        QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, TEST_TIMEOUT_MS);
        QCOMPARE(spy[0][0].toULongLong(), quint64(33));
        QVERIFY(!qvariant_cast<Result>(spy[0][1]).success);
        QCOMPARE(client.pendingCount(), 32);
        client.cancel();
        QVERIFY(!client.busy());
        QCOMPARE(spy.size(), 1);
    }

    void isolatedModeCannotSubmitToProduction()
    {
        auto profile = HttpFixture().profile();
        profile.endpoint = QUrl(QStringLiteral("https://uom.receive.caacic.cn/ridData"));
        UomReportClient client;
        QSignalSpy spy(&client, &UomReportClient::completed);
        client.send(1, profile, QStringLiteral("test-key"), ORDER_ID, BODY);
        QVERIFY(!client.busy());
        QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, TEST_TIMEOUT_MS);
        QVERIFY(!qvariant_cast<Result>(spy[0][1]).success);
    }
};

QTEST_GUILESS_MAIN(UomReportClientTest)
#include "UomReportClientTest.moc"
