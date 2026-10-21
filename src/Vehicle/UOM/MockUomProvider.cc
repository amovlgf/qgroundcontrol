#include "MockUomProvider.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

MockUomProvider::MockUomProvider(const QUrl& endpoint, QObject* parent)
    : UomProvider(parent)
    , _endpoint(endpoint)
{
    _network.setProxy(QNetworkProxy::NoProxy);
}

bool MockUomProvider::isLocalEndpoint(const QUrl& endpoint)
{
    return endpoint.isValid() && endpoint.scheme() == QStringLiteral("http") && endpoint.userInfo().isEmpty() &&
           !endpoint.hasQuery() && !endpoint.hasFragment() &&
           (endpoint.host() == QStringLiteral("localhost") || QHostAddress(endpoint.host()).isLoopback());
}

void MockUomProvider::authenticate(quint64 generation, const Uom::Identity& identity)
{
    _request(true, generation, 0, identity, {});
}

void MockUomProvider::submit(quint64 generation, quint64 sequence, const Uom::Identity& identity,
                             const Uom::Telemetry& telemetry)
{
    _request(false, generation, sequence, identity, telemetry.toJson());
}

void MockUomProvider::cancel()
{
    ++_operation;
    _token.clear();
    if (_reply) {
        _reply->abort();
        _reply->deleteLater();
        _reply.clear();
    }
}

void MockUomProvider::_request(bool authentication, quint64 generation, quint64 sequence, const Uom::Identity& identity,
                               const QJsonObject& payload)
{
    const auto finish = [this, authentication, generation, sequence](bool accepted, Uom::Fault fault,
                                                                     const QString& detail) {
        if (authentication) {
            emit authenticated(generation, accepted, fault, detail);
        } else {
            emit acknowledged(generation, sequence, accepted, fault, detail);
        }
    };
    if (!isLocalEndpoint(_endpoint) || _reply) {
        QTimer::singleShot(0, this, [finish]() {
            finish(false, Uom::Fault::Network, tr("Mock UOM requires an idle localhost HTTP endpoint"));
        });
        return;
    }
    QUrl url(_endpoint);
    url.setPath(authentication ? QStringLiteral("/uom-test-v1/auth") : QStringLiteral("/uom-test-v1/report"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(2000);
    if (!authentication) {
        request.setRawHeader("Authorization", "Bearer " + _token);
    }
    const QString digest = QString::fromLatin1(identity.digest.toHex());
    const QJsonObject body{{"protocol", "uom-test-v1"},
                           {"environment", "mock"},
                           {"generation", QString::number(generation)},
                           {"sequence", QString::number(sequence)},
                           {"identity", identity.toJson()},
                           {"telemetry", payload}};
    const quint64 operation = ++_operation;
    QNetworkReply* const reply = _network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    _reply = reply;
    connect(reply, &QNetworkReply::readyRead, this, [reply]() {
        if (reply->bytesAvailable() > 16384) {
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, operation, authentication, generation, sequence, digest, finish]() {
                reply->deleteLater();
                if (operation != _operation) {
                    return;
                }
                _reply.clear();
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (status == 401 || status == 403) {
                    _token.clear();
                    finish(false, Uom::Fault::Authentication, tr("Mock authorization rejected"));
                    return;
                }
                if (reply->error() != QNetworkReply::NoError || status != 200) {
                    finish(false, Uom::Fault::Network, tr("Mock request failed (%1)").arg(status));
                    return;
                }
                const QByteArray bytes = reply->readAll();
                QJsonParseError error;
                const QJsonObject result = QJsonDocument::fromJson(bytes, &error).object();
                const bool correlated = bytes.size() <= 16384 && error.error == QJsonParseError::NoError &&
                                        result.value("protocol").toString() == QStringLiteral("uom-test-v1") &&
                                        result.value("environment").toString() == QStringLiteral("mock") &&
                                        result.value("generation").toString() == QString::number(generation) &&
                                        result.value("sequence").toString() == QString::number(sequence) &&
                                        result.value("identity_digest").toString() == digest;
                if (!correlated || !result.value("accepted").toBool()) {
                    finish(false, Uom::Fault::Response,
                           tr("Mock response rejected or does not match this aircraft session"));
                    return;
                }
                if (authentication) {
                    _token = result.value("token").toString().toUtf8();
                    if (_token.isEmpty() || _token.size() > 1024 || _token.contains('\r') || _token.contains('\n')) {
                        _token.clear();
                        finish(false, Uom::Fault::Response, tr("Mock authentication returned an invalid token"));
                        return;
                    }
                }
                finish(true, Uom::Fault::None, tr("Simulation acknowledged; not official UOM"));
            });
}
