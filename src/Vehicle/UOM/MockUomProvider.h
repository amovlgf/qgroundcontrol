#pragma once

#include <QtCore/QPointer>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>

#include "UomProvider.h"

class QNetworkReply;

/// The uom-test-v1 protocol is a local test protocol, never an implementation of official UOM.
class MockUomProvider final : public UomProvider
{
    Q_OBJECT
public:
    explicit MockUomProvider(const QUrl& endpoint, QObject* parent = nullptr);

    Uom::Environment environment() const override { return Uom::Environment::Mock; }

    void authenticate(quint64 generation, const Uom::Identity& identity) override;
    void submit(quint64 generation, quint64 sequence, const Uom::Identity& identity,
                const Uom::Telemetry& telemetry) override;
    void cancel() override;
    static bool isLocalEndpoint(const QUrl& endpoint);

private:
    void _request(bool authentication, quint64 generation, quint64 sequence, const Uom::Identity& identity,
                  const QJsonObject& payload);

    QUrl _endpoint;
    QNetworkAccessManager _network;
    QPointer<QNetworkReply> _reply;
    QByteArray _token;
    quint64 _operation = 0;
};
