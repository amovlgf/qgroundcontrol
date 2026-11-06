#include <limits>

#include <QtCore/QJsonDocument>
#include <QtTest/QTest>

#include "UomReportProtocol.h"

using namespace UomReporting;

namespace {
const QByteArray GOLDEN_BODY =
    "FF2048FFFFFE31353831463435543031323334353637383941423132333435363738010100A0A2CF44D8F3F417B009"
    "A0A2CF44D8F3F417080A1900544716280A140A010001010108573BD79B0101";

Sample example()
{
    Sample sample;
    sample.aircraftId = QStringLiteral("1581F45T0123456789AB");
    sample.registrationId = QStringLiteral("12345678");
    sample.operationCategory = 1;
    sample.aircraftClass = 1;
    sample.operatorLocationType = 0;
    sample.longitude = sample.operatorLongitude = 115.4458272;
    sample.latitude = sample.operatorLatitude = 40.1929176;
    sample.operatorAltitudeEllipsoid = 240;
    sample.trackDegrees = 256.8;
    sample.groundSpeed = 2.5;
    sample.relativeAltitude = 130;
    sample.verticalSpeedUp = 11;
    sample.altitudeEllipsoid = 300;
    sample.pressureAltitude = 290;
    sample.operationState = 1;
    sample.horizontalAccuracy = sample.verticalAccuracy = sample.speedAccuracy = sample.timestampAccuracy = 1;
    sample.timestampMs = 1768842549000;
    return sample;
}

Profile profile()
{
    return {.mode = Mode::Isolated,
            .endpoint = QUrl(QStringLiteral("http://127.0.0.1:12345/ridData")),
            .appId = QStringLiteral("test-app"),
            .uasModel = QStringLiteral("TEST-MODEL"),
            .uscCode = QStringLiteral("TEST-USC"),
            .platform = QStringLiteral("QGC-test"),
            .credentialReference = QStringLiteral("test-key-reference")};
}
}  // namespace

class UomReportProtocolTest : public QObject
{
    Q_OBJECT
private slots:

    void officialExample()
    {
        const auto encoded = encode(example());
        QVERIFY(encoded);
        QCOMPARE(encoded->size(), 78);
        QCOMPARE(encoded->toHex().toUpper(), GOLDEN_BODY);
        const auto decoded = decode(QByteArray::fromHex(GOLDEN_BODY));
        QVERIFY(decoded);
        QCOMPARE(decoded->toJson(), example().toJson());
        QCOMPARE(Sample::fromJson(decoded->toJson())->toJson(), decoded->toJson());
    }

    void signatureUsesAsciiHexAndRawUtf8Key()
    {
        // Independent Python hashlib/hmac SM3 vectors; a key containing hex digits must remain UTF-8.
        QCOMPARE(*signBody(GOLDEN_BODY, QStringLiteral("test-app-key")),
                 QByteArray("294c1a1ec137b651ec704642283f462cea5156a9cc62cfc9788ed943f76500fa"));
        QCOMPARE(*signBody(GOLDEN_BODY, QStringLiteral("0123456789abcdef")),
                 QByteArray("c34397c2b8a255d0a116021bfe6376844e71862a746148ff4deb24b6fcacab91"));
        QCOMPARE(*signBody(GOLDEN_BODY, QStringLiteral("测试密钥")),
                 QByteArray("ef8ab2a623fb5ad1f34819b9492a3eee94c0cd9881ccd3248cba97f84b607e79"));
        QVERIFY(!signBody(GOLDEN_BODY.toLower(), QStringLiteral("test-app-key")));
        QVERIFY(!signBody(GOLDEN_BODY + '\n', QStringLiteral("test-app-key")));
        QVERIFY(!signBody(QByteArray::fromHex(GOLDEN_BODY), QStringLiteral("test-app-key")));
        QVERIFY(!signBody(GOLDEN_BODY, {}));
    }

    void unknownAndOptionalFields()
    {
        Sample sample;
        sample.aircraftId = QStringLiteral("OLD123");
        const auto packet = encode(sample);
        QVERIFY(packet);
        QCOMPARE(packet->mid(6, 20), QByteArray(14, '\0') + QByteArray("OLD123"));
        QCOMPARE(packet->mid(26, 8), QByteArray(8, '\0'));
        QCOMPARE(packet->mid(37, 8), QByteArray(8, char(0xff)));
        QCOMPARE(packet->mid(45, 2), QByteArray(2, '\0'));
        QCOMPARE(packet->mid(55, 4), QByteArray(4, char(0xff)));
        QCOMPARE(quint8(packet->at(61)), quint8(0xff));
        QCOMPARE(packet->mid(71, 6), QByteArray(6, '\0'));
        const auto decoded = decode(*packet);
        QVERIFY(decoded);
        QCOMPARE(decoded->toJson(), sample.toJson());
        sample.fields = REQUIRED_FIELDS;
        const auto minimal = encode(sample);
        QVERIFY(minimal);
        QCOMPARE(minimal->size(), 72);
        QCOMPARE(minimal->mid(3, 3).toHex(), QByteArray("dfe5fe"));
        QCOMPARE(decode(*minimal)->fields, REQUIRED_FIELDS);
        QCOMPARE(encode(*decode(*minimal)), minimal);
    }

    void signedCoordinatesAndQuantization()
    {
        Sample sample = example();
        sample.longitude = -122.1234567;
        sample.latitude = -45.9876543;
        sample.verticalSpeedUp = -4.9;
        sample.trackDegrees = 359.99;
        sample.groundSpeed = 4.29;
        sample.relativeAltitude = -5.25;
        sample.altitudeEllipsoid = -12.25;
        sample.timestampMs = MAX_TIMESTAMP_MS;
        const auto decoded = decode(*encode(sample));
        QVERIFY(decoded);
        QCOMPARE(decoded->longitude, sample.longitude);
        QCOMPARE(decoded->latitude, sample.latitude);
        QCOMPARE(*decoded->verticalSpeedUp, -4.5);
        QCOMPARE(*decoded->trackDegrees, 359.9);
        QCOMPARE(*decoded->groundSpeed, 4.2);
        QCOMPARE(*decoded->relativeAltitude, -5.5);
        QCOMPARE(*decoded->altitudeEllipsoid, -12.5);
        QCOMPARE(decoded->timestampMs, MAX_TIMESTAMP_MS);
    }

    void invalidInputs_data()
    {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QJsonValue>("value");
        QTest::newRow("oversize-identity") << QString("aircraftId") << QJsonValue(QString(21, 'A'));
        QTest::newRow("unicode-identity") << QString("aircraftId") << QJsonValue(QStringLiteral("无人机"));
        QTest::newRow("registration-size") << QString("registrationId") << QJsonValue("123456789");
        QTest::newRow("mandatory-bit") << QString("fields") << QJsonValue(int(ALL_FIELDS & ~1U));
        QTest::newRow("latitude") << QString("latitude") << QJsonValue(90.0001);
        QTest::newRow("longitude") << QString("longitude") << QJsonValue(-180.0001);
        QTest::newRow("reserved-coordinate") << QString("longitude") << QJsonValue(-0.0000001);
        QTest::newRow("altitude-sentinel") << QString("altitudeEllipsoid") << QJsonValue(-1000);
        QTest::newRow("track") << QString("trackDegrees") << QJsonValue(360);
        QTest::newRow("negative-speed") << QString("groundSpeed") << QJsonValue(-0.1);
        QTest::newRow("vertical-sentinel") << QString("verticalSpeedUp") << QJsonValue(-63.5);
        QTest::newRow("accuracy") << QString("horizontalAccuracy") << QJsonValue(13);
        QTest::newRow("fractional-enum") << QString("aircraftClass") << QJsonValue(1.5);
        QTest::newRow("timestamp") << QString("timestampMs") << QJsonValue(QString::number(MAX_TIMESTAMP_MS + 1));
        QTest::newRow("unknown-version") << QString("schema") << QJsonValue(2);
    }

    void invalidInputs()
    {
        QFETCH(QString, field);
        QFETCH(QJsonValue, value);
        auto json = example().toJson();
        json[field] = value;
        QVERIFY(!Sample::fromJson(json));
    }

    void nonFiniteValuesAndMalformedPackets()
    {
        auto sample = example();
        sample.groundSpeed = std::numeric_limits<double>::quiet_NaN();
        QVERIFY(!encode(sample));
        sample.groundSpeed = std::numeric_limits<double>::infinity();
        QVERIFY(!encode(sample));
        const auto packet = QByteArray::fromHex(GOLDEN_BODY);
        for (int i = 0; i < packet.size(); ++i) {
            QVERIFY(!decode(packet.left(i)));
        }
        for (const auto index : {0, 1, 2, 3, 4, 5}) {
            auto damaged = packet;
            damaged[index] = char(quint8(damaged[index]) ^ 1);
            QVERIFY(!decode(damaged));
        }
        QVERIFY(!decode(packet + '\0'));
    }

    void requestAndProfileIsolation()
    {
        auto cfg = profile();
        QVERIFY(cfg.valid());
        QCOMPARE(Profile::fromJson(cfg.toJson())->toJson(), cfg.toJson());
        auto request =
            buildRequest(cfg, QStringLiteral("test-app-key"), QStringLiteral("20260120-AbCd1234"), GOLDEN_BODY);
        QVERIFY(request);
        QCOMPARE(request->rawHeader("source"), QByteArray("1"));
        QCOMPARE(request->rawHeader("sign"), *signBody(GOLDEN_BODY, QStringLiteral("test-app-key")));
        QCOMPARE(request->header(QNetworkRequest::ContentTypeHeader).toString(),
                 QStringLiteral("application/octet-stream"));
        QCOMPARE(request->attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(),
                 int(QNetworkRequest::ManualRedirectPolicy));
        QVERIFY(!QJsonDocument(cfg.toJson()).toJson().contains("test-app-key"));
        QVERIFY(!buildRequest(cfg, QStringLiteral("key"), QStringLiteral("20260120-bad"), GOLDEN_BODY));
        cfg.endpoint = QUrl(QStringLiteral("https://uom.receive.caacic.cn/ridData"));
        QVERIFY(!cfg.valid());
        cfg.mode = Mode::Commissioning;
        QVERIFY(cfg.valid());
        cfg.mode = Mode::Official;
        QVERIFY(cfg.valid());
        for (const auto* endpoint : {"http://uom.receive.caacic.cn/ridData", "https://uom.receive.caacic.cn/wrong",
                                     "https://evil.test/ridData", "https://uom.receive.caacic.cn/ridData?key=bad"}) {
            cfg.endpoint = QUrl(QString::fromLatin1(endpoint));
            QVERIFY(!cfg.valid());
        }
        cfg = profile();
        cfg.appId += QStringLiteral("\r\ninjected:value");
        QVERIFY(!cfg.valid());
    }

    void response_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<bool>("success");
        QTest::addColumn<bool>("retryable");
        QTest::addColumn<bool>("authentication");
        QTest::newRow("success-number") << 200 << QByteArray(R"({"code":200,"msg":"success"})") << true << false
                                        << false;
        QTest::newRow("success-string") << 200 << QByteArray(R"({"code":"200","msg":"success"})") << true << false
                                        << false;
        QTest::newRow("not-http200") << 201 << QByteArray(R"({"code":200,"msg":"success"})") << false << false << false;
        QTest::newRow("business-rejected")
            << 200 << QByteArray(R"({"code":400,"msg":"bad"})") << false << false << false;
        QTest::newRow("business-auth") << 200 << QByteArray(R"({"code":"401","msg":"bad"})") << false << false << true;
        QTest::newRow("http-auth") << 403 << QByteArray() << false << false << true;
        QTest::newRow("http-server") << 500 << QByteArray() << false << true << false;
        QTest::newRow("business-server") << 200 << QByteArray(R"({"code":500,"msg":"bad"})") << false << true << false;
        QTest::newRow("http429") << 429 << QByteArray() << false << true << false;
        QTest::newRow("fractional-code") << 200 << QByteArray(R"({"code":200.5,"msg":"success"})") << false << false
                                         << false;
        QTest::newRow("missing-message") << 200 << QByteArray(R"({"code":200})") << false << false << false;
        QTest::newRow("malformed") << 200 << QByteArray("broken") << false << false << false;
        QTest::newRow("oversize") << 200 << QByteArray(MAX_RESPONSE_BYTES + 1, 'x') << false << false << false;
    }

    void response()
    {
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        QFETCH(bool, success);
        QFETCH(bool, retryable);
        QFETCH(bool, authentication);
        const auto result = parseResponse(status, body);
        QCOMPARE(result.success, success);
        QCOMPARE(result.retryable, retryable);
        QCOMPARE(result.authenticationFailure, authentication);
        QCOMPARE(result.httpStatus, status);
    }
};

QTEST_GUILESS_MAIN(UomReportProtocolTest)
#include "UomReportProtocolTest.moc"
