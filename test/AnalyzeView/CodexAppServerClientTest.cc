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

void CodexAppServerClientTest::_proxyEnvironment_test()
{
    QProcessEnvironment environment;
    environment.insert(QStringLiteral("PATH"), QStringLiteral("keep-this-path"));
    environment.insert(QStringLiteral("HTTPS_PROXY"), QStringLiteral("http://old-proxy:8080"));
    environment.insert(QStringLiteral("https_proxy"), QStringLiteral("http://another-proxy:8081"));
    environment.insert(QStringLiteral("NO_PROXY"), QStringLiteral("*"));
    environment.insert(QStringLiteral("no_proxy"), QStringLiteral("auth.openai.com"));
    const QProcessEnvironment original = environment;
    QString error;

    QVERIFY(CodexAppServerClient::_applyProxyEnvironment(environment, QStringLiteral("  "), &error));
    QCOMPARE(environment, original);

    for (const QString &url : {QStringLiteral("http://127.0.0.1:7890"), QStringLiteral("https://proxy.example:8443"),
                              QStringLiteral("socks5://[::1]:1080"), QStringLiteral("socks5h://user:p%40ss@proxy.example:1080")}) {
        QVERIFY(CodexAppServerClient::_applyProxyEnvironment(environment, QStringLiteral("  %1  ").arg(url), &error));
        QVERIFY(error.isEmpty());
        for (const QString &name : {QStringLiteral("HTTP_PROXY"), QStringLiteral("HTTPS_PROXY"), QStringLiteral("ALL_PROXY")}) {
            QCOMPARE(environment.value(name), url);
            QCOMPARE(environment.value(name.toLower()), url);
        }
        QCOMPARE(environment.value(QStringLiteral("PATH")), original.value(QStringLiteral("PATH")));
        QCOMPARE(environment.value(QStringLiteral("NO_PROXY")), QStringLiteral("localhost,127.0.0.1,::1"));
        QCOMPARE(environment.value(QStringLiteral("no_proxy")), environment.value(QStringLiteral("NO_PROXY")));
    }
}

void CodexAppServerClientTest::_invalidProxyUrl_data()
{
    QTest::addColumn<QString>("url");
    QTest::newRow("missing-scheme") << QStringLiteral("127.0.0.1:7890");
    QTest::newRow("unsupported-scheme") << QStringLiteral("file:///tmp/proxy");
    QTest::newRow("missing-host") << QStringLiteral("http://:7890");
    QTest::newRow("invalid-port") << QStringLiteral("http://proxy:70000");
    QTest::newRow("zero-port") << QStringLiteral("http://proxy:0");
    QTest::newRow("invalid-host") << QStringLiteral("http://bad host:7890");
    QTest::newRow("path") << QStringLiteral("http://proxy:7890/api");
    QTest::newRow("query") << QStringLiteral("http://proxy:7890?token=secret");
    QTest::newRow("fragment") << QStringLiteral("http://proxy:7890#fragment");
    QTest::newRow("credentials-redacted") << QStringLiteral("ftp://private-user:private-password@proxy:7890");
}

void CodexAppServerClientTest::_invalidProxyUrl()
{
    QFETCH(QString, url);
    QProcessEnvironment environment;
    environment.insert(QStringLiteral("HTTPS_PROXY"), QStringLiteral("http://existing:8080"));
    const QProcessEnvironment original = environment;
    QString error;
    QVERIFY(!CodexAppServerClient::_applyProxyEnvironment(environment, url, &error));
    QCOMPARE(environment, original);
    QVERIFY(!error.isEmpty());
    QVERIFY(!error.contains(url));
    QVERIFY(!error.contains(QStringLiteral("private-password")));
    QVERIFY(!error.contains(QStringLiteral("private-user")));
}

void CodexAppServerClientTest::_deviceCodeForbiddenError_test()
{
    const QString message = CodexAppServerClient::deviceCodeLoginErrorText(
        QStringLiteral("failed to request device code: device code request failed with status 403 Forbidden"));
    QVERIFY(message.contains(QStringLiteral("403")));
    QVERIFY(message.contains(QStringLiteral("proxy")));
    QVERIFY(message.contains(QStringLiteral("restart")));
    QCOMPARE(CodexAppServerClient::deviceCodeLoginErrorText(QStringLiteral("connection timed out")),
             QStringLiteral("Authorization failed: connection timed out"));
    QCOMPARE(CodexAppServerClient::deviceCodeLoginErrorText(QStringLiteral("account access denied: 403 Forbidden")),
             QStringLiteral("Authorization failed: account access denied: 403 Forbidden"));
}
