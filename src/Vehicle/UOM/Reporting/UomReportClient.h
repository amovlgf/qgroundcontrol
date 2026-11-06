#pragma once

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QPointer>

#include "UomReportProtocol.h"

class QNetworkAccessManager;
class QNetworkReply;

/// Requests carry frozen bodies; retries and aircraft/session ownership belong to the caller.
class UomReportClient final : public QObject
{
    Q_OBJECT
public:
    explicit UomReportClient(QObject* parent = nullptr);
    UomReportClient(QNetworkAccessManager* network, QObject* parent, int timeoutMs = 3000);
    ~UomReportClient() override;

    void send(quint64 requestId, const UomReporting::Profile& profile, const QString& appKey, const QString& orderId,
              const QByteArray& body);
    void cancel();
    void cancel(quint64 requestId);

    [[nodiscard]] bool busy() const { return !_replies.isEmpty(); }

    [[nodiscard]] qsizetype pendingCount() const { return _replies.size(); }

signals:
    void completed(quint64 requestId, const UomReporting::Result& result);

private:
    void _reject(quint64 requestId, const QString& detail);
    QNetworkAccessManager* _network;
    QHash<quint64, QPointer<QNetworkReply>> _replies;
    quint64 _generation = 0;
    int _timeoutMs;
};
