/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "CodexAppServerClientTest.h"

#include "CodexAppServerClient.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

void CodexAppServerClientTest::_jsonlDecoderHandlesPartialAndMultipleLines_test()
{
    CodexAppServerClient client;
    QSignalSpy notificationSpy(&client, &CodexAppServerClient::notificationReceived);

    client.feedStandardOutput(QByteArrayLiteral("{\"method\":\"account/updated\",\"params\":{"));
    QCOMPARE(notificationSpy.count(), 0);
    client.feedStandardOutput(QByteArrayLiteral("\"planType\":\"plus\"}}\n"));
    QCOMPARE(notificationSpy.count(), 1);
    QCOMPARE(notificationSpy.at(0).at(0).toString(), QStringLiteral("account/updated"));

    client.feedStandardOutput(QByteArrayLiteral("{\"method\":\"one\"}\n{\"method\":\"two\"}\n"));
    QCOMPARE(notificationSpy.count(), 3);
}

void CodexAppServerClientTest::_invalidJsonIsReported_test()
{
    CodexAppServerClient client;
    QSignalSpy errorSpy(&client, &CodexAppServerClient::protocolError);
    client.feedStandardOutput(QByteArrayLiteral("not-json\n"));
    QCOMPARE(errorSpy.count(), 1);
}

void CodexAppServerClientTest::_requestsAreBlockedBeforeInitialize_test()
{
    CodexAppServerClient client;
    QList<QByteArray> writes;
    client.setTestTransport([&writes](const QByteArray &line) { writes.append(line); });

    QCOMPARE(client.request(QStringLiteral("account/read"), QJsonObject(), [](const QJsonValue &, const QJsonObject &) {}), qint64(-1));
    QVERIFY(writes.isEmpty());
}

void CodexAppServerClientTest::_requestResponseIdsAndErrors_test()
{
    CodexAppServerClient client;
    QList<QByteArray> writes;
    client.setTestTransport([&writes](const QByteArray &line) { writes.append(line); });
    client.setProtocolReadyForTesting(true);

    bool responseReceived = false;
    bool errorReceived = false;
    const qint64 requestId = client.request(QStringLiteral("account/read"), QJsonObject(), [&responseReceived, &errorReceived](const QJsonValue &result, const QJsonObject &error) {
        responseReceived = result.toObject().value(QStringLiteral("ok")).toBool();
        errorReceived = !error.isEmpty();
    });
    QVERIFY(requestId > 0);
    const QJsonObject request = QJsonDocument::fromJson(writes.takeFirst()).object();
    QCOMPARE(static_cast<qint64>(request.value(QStringLiteral("id")).toDouble()), requestId);

    client.feedStandardOutput(QJsonDocument(QJsonObject{
        { QStringLiteral("id"), requestId + 100 },
        { QStringLiteral("result"), QJsonObject{ { QStringLiteral("ok"), false } } }
    }).toJson(QJsonDocument::Compact) + '\n');
    QVERIFY(!responseReceived);

    client.feedStandardOutput(QJsonDocument(QJsonObject{
        { QStringLiteral("id"), requestId },
        { QStringLiteral("result"), QJsonObject{ { QStringLiteral("ok"), true } } }
    }).toJson(QJsonDocument::Compact) + '\n');
    QVERIFY(responseReceived);
    QVERIFY(!errorReceived);

    bool rpcErrorReceived = false;
    const qint64 errorRequestId = client.request(QStringLiteral("model/list"), QJsonObject(), [&rpcErrorReceived](const QJsonValue &, const QJsonObject &error) {
        rpcErrorReceived = error.value(QStringLiteral("message")).toString() == QStringLiteral("unsupported");
    });
    client.feedStandardOutput(QJsonDocument(QJsonObject{
        { QStringLiteral("id"), errorRequestId },
        { QStringLiteral("error"), QJsonObject{
            { QStringLiteral("code"), -32601 },
            { QStringLiteral("message"), QStringLiteral("unsupported") }
        } }
    }).toJson(QJsonDocument::Compact) + '\n');
    QVERIFY(rpcErrorReceived);
}

void CodexAppServerClientTest::_requestTimeout_test()
{
    CodexAppServerClient client;
    client.setTestTransport([](const QByteArray &) {});
    client.setProtocolReadyForTesting(true);
    bool timedOut = false;
    client.request(QStringLiteral("model/list"), QJsonObject(), [&timedOut](const QJsonValue &, const QJsonObject &error) {
        timedOut = error.value(QStringLiteral("message")).toString().contains(QStringLiteral("timed out"));
    }, 20);
    QTRY_VERIFY_WITH_TIMEOUT(timedOut, 1000);
}

void CodexAppServerClientTest::_approvalRequestsAreDeclined_test()
{
    CodexAppServerClient client;
    QList<QByteArray> writes;
    client.setTestTransport([&writes](const QByteArray &line) { writes.append(line); });
    client.setProtocolReadyForTesting(true);
    client.feedStandardOutput(QByteArrayLiteral("{\"id\":9,\"method\":\"item/commandExecution/requestApproval\",\"params\":{}}\n"));
    QCOMPARE(writes.size(), 1);
    const QJsonObject response = QJsonDocument::fromJson(writes.first()).object();
    QCOMPARE(response.value(QStringLiteral("id")).toInt(), 9);
    QCOMPARE(response.value(QStringLiteral("result")).toObject().value(QStringLiteral("decision")).toString(), QStringLiteral("decline"));
}

void CodexAppServerClientTest::_accountLoginAndModelSelection_test()
{
    QString loginId;
    QString verificationUrl;
    QString userCode;
    QVERIFY(CodexAppServerClient::parseDeviceCodeLoginResponse(QJsonObject{
        { QStringLiteral("loginId"), QStringLiteral("login-1") },
        { QStringLiteral("verificationUrl"), QStringLiteral("https://example.test/device") },
        { QStringLiteral("userCode"), QStringLiteral("ABCD-1234") }
    }, &loginId, &verificationUrl, &userCode));
    QCOMPARE(loginId, QStringLiteral("login-1"));
    QCOMPARE(verificationUrl, QStringLiteral("https://example.test/device"));
    QCOMPARE(userCode, QStringLiteral("ABCD-1234"));

    QString email;
    QString planType;
    QVERIFY(CodexAppServerClient::parseChatGptAccountResponse(QJsonObject{
        { QStringLiteral("account"), QJsonObject{
            { QStringLiteral("type"), QStringLiteral("chatgpt") },
            { QStringLiteral("email"), QStringLiteral("pilot@example.test") },
            { QStringLiteral("planType"), QStringLiteral("plus") }
        } }
    }, &email, &planType));
    QCOMPARE(email, QStringLiteral("pilot@example.test"));
    QCOMPARE(planType, QStringLiteral("plus"));

    const QVariantList models = CodexAppServerClient::parseModelListResponse(QJsonObject{
        { QStringLiteral("data"), QJsonArray{
            QJsonObject{{ QStringLiteral("model"), QStringLiteral("first") }, { QStringLiteral("displayName"), QStringLiteral("First") }, { QStringLiteral("isDefault"), false }},
            QJsonObject{{ QStringLiteral("model"), QStringLiteral("default") }, { QStringLiteral("displayName"), QStringLiteral("Default") }, { QStringLiteral("isDefault"), true }}
        } }
    });
    QCOMPARE(CodexAppServerClient::preferredModelIndex(models, QStringLiteral("first")), 0);
    QCOMPARE(CodexAppServerClient::preferredModelIndex(models, QStringLiteral("missing")), 1);
}
