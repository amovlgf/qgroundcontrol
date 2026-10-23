#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include "RidActivationProvider.h"

class RidActivationProviderTest final : public QObject
{
    Q_OBJECT
private slots:

    void stateMapping_data()
    {
        QTest::addColumn<QJsonValue>("state");
        QTest::addColumn<int>("expected");
        QTest::newRow("active") << QJsonValue("1") << int(RidActivationResult::State::Active);
        QTest::newRow("inactive") << QJsonValue("2") << int(RidActivationResult::State::Inactive);
        QTest::newRow("not-reported") << QJsonValue("") << int(RidActivationResult::State::Unknown);
        QTest::newRow("numeric") << QJsonValue(1) << int(RidActivationResult::State::Unknown);
        QTest::newRow("null") << QJsonValue(QJsonValue::Null) << int(RidActivationResult::State::Unknown);
        QTest::newRow("unknown") << QJsonValue("0") << int(RidActivationResult::State::Unknown);
        QTest::newRow("whitespace") << QJsonValue("1 ") << int(RidActivationResult::State::Unknown);
    }

    void stateMapping()
    {
        QFETCH(QJsonValue, state);
        QFETCH(int, expected);
        auto data = _data();
        data["activation_state"] = state;
        const auto result = RidActivationProvider::parseResponse(SERIAL, 200, _body(data));
        QCOMPARE(int(result.state), expected);
    }

    void identityAndTransportRejections()
    {
        auto data = _data();
        data["sn"] = "OTHER000000000000000";
        QVERIFY(!RidActivationProvider::parseResponse(SERIAL, 200, _body(data)).writable());
        data = _data();
        data["unique_product_code"] = "OTHER000000000000000";
        QVERIFY(!RidActivationProvider::parseResponse(SERIAL, 200, _body(data)).writable());
        data = _data();
        data["bound"] = "true";
        QVERIFY(!RidActivationProvider::parseResponse(SERIAL, 200, _body(data)).writable());
        data = _data();
        data["bound"] = false;
        QCOMPARE(RidActivationProvider::parseResponse(SERIAL, 200, _body(data)).state,
                 RidActivationResult::State::Unbound);
        QVERIFY(!RidActivationProvider::parseResponse(SERIAL, 502, _body(_data())).writable());
        QVERIFY(!RidActivationProvider::parseResponse(SERIAL, 200, "invalid").writable());
        QVERIFY(!RidActivationProvider::parseResponse(SERIAL, 200, "{}").writable());
        QVERIFY(!RidActivationProvider::validSerial("SHORT"));
        QVERIFY(!RidActivationProvider::validSerial(QString(20, u' ')));
    }

    void httpQueryAndCancellation()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QByteArray received;
        connect(&server, &QTcpServer::newConnection, this, [&]() {
            auto* socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, this, [&, socket]() {
                received += socket->readAll();
                if (received.contains("\r\n\r\n") && received.contains(SERIAL.toLatin1())) {
                    const QByteArray body = _body(_data());
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                  QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                }
            });
        });
        RidActivationProvider provider(
            QUrl(QStringLiteral("http://127.0.0.1:%1/api/v1/device/check-binding").arg(server.serverPort())), nullptr);
        QSignalSpy result(&provider, &RidActivationProvider::resultReady);
        provider.query(42, SERIAL);
        QTRY_COMPARE_WITH_TIMEOUT(result.count(), 1, 5000);
        QCOMPARE(result.at(0).at(0).toULongLong(), quint64(42));
        QCOMPARE(qvariant_cast<RidActivationResult>(result.at(0).at(1)).state, RidActivationResult::State::Active);
        QVERIFY(received.startsWith("POST /api/v1/device/check-binding HTTP/1.1"));
        QVERIFY(!received.contains("report-active"));
        result.clear();
        received.clear();
        provider.query(43, SERIAL);
        provider.cancel();
        QCoreApplication::processEvents();
        QCOMPARE(result.count(), 0);
    }

private:
    inline static const QString SERIAL = QStringLiteral("AMOV0123456789012345");

    static QJsonObject _data()
    {
        return {{"sn", SERIAL}, {"bound", true}, {"unique_product_code", SERIAL}, {"activation_state", "1"}};
    }

    static QByteArray _body(const QJsonObject& data)
    {
        return QJsonDocument(QJsonObject{{"data", data}}).toJson(QJsonDocument::Compact);
    }
};

QTEST_GUILESS_MAIN(RidActivationProviderTest)
#include "RidActivationProviderTest.moc"
