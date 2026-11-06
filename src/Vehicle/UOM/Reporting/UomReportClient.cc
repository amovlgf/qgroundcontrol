#include "UomReportClient.h"

#include <memory>

#include <QtCore/QTimer>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkReply>

UomReportClient::UomReportClient(QObject* parent)
    : UomReportClient(new QNetworkAccessManager, parent)
{
    _network->setParent(this);
    _network->setProxy(QNetworkProxy::NoProxy);
}

UomReportClient::UomReportClient(QNetworkAccessManager* network, QObject* parent, int timeoutMs)
    : QObject(parent)
    , _network(network)
    , _timeoutMs(qMax(timeoutMs, 1))
{}

UomReportClient::~UomReportClient()
{
    cancel();
}

void UomReportClient::_reject(quint64 requestId, const QString& detail)
{
    const quint64 generation = _generation;
    QTimer::singleShot(0, this, [this, requestId, detail, generation]() {
        if (generation == _generation) {
            emit completed(requestId, UomReporting::Result{.detail = detail});
        }
    });
}

void UomReportClient::send(quint64 requestId, const UomReporting::Profile& profile, const QString& appKey,
                           const QString& orderId, const QByteArray& body)
{
    if (_replies.contains(requestId)) {
        return;  // A duplicate must not emit a completion for the existing request.
    }
    if (!_network || _replies.size() >= 32) {
        _reject(requestId, tr("UOM request is already pending or capacity is exhausted"));
        return;
    }
    auto request = UomReporting::buildRequest(profile, appKey, orderId, body);
    if (!request) {
        _reject(requestId, tr("Invalid UOM reporting profile, identity, payload or credential"));
        return;
    }
    request->setTransferTimeout(_timeoutMs);
    const quint64 generation = _generation;
    QNetworkReply* reply = _network->post(*request, body);
    reply->setReadBufferSize(UomReporting::MAX_RESPONSE_BYTES + 1);
    _replies.insert(requestId, reply);

    struct ResponseState
    {
        QByteArray body;
        bool oversized = false;
        bool timedOut = false;
    };

    const auto state = std::make_shared<ResponseState>();
    const auto read = [reply, state]() {
        if (state->oversized || !reply->isOpen()) {
            return;
        }
        const qint64 remaining = UomReporting::MAX_RESPONSE_BYTES - state->body.size();
        state->body += reply->read(remaining + 1);
        if (state->body.size() > UomReporting::MAX_RESPONSE_BYTES || reply->bytesAvailable() > 0) {
            state->oversized = true;
            reply->abort();
        }
    };
    connect(reply, &QNetworkReply::readyRead, this, read);
    QTimer::singleShot(_timeoutMs, reply, [reply, state]() {
        if (!reply->isFinished()) {
            state->timedOut = true;
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, requestId, generation, state, read]() {
        reply->deleteLater();
        if (generation != _generation || _replies.value(requestId) != reply) {
            return;
        }
        _replies.remove(requestId);
        read();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        UomReporting::Result result;
        if (state->oversized) {
            result = {.httpStatus = status, .detail = tr("UOM response exceeded the size limit")};
        } else if (state->timedOut || (reply->error() != QNetworkReply::NoError && (status == 0 || status == 200))) {
            result = {.retryable = true, .httpStatus = status, .detail = tr("UOM report has no confirmed response")};
        } else {
            result = UomReporting::parseResponse(status, state->body);
        }
        emit completed(requestId, result);
    });
}

void UomReportClient::cancel()
{
    ++_generation;
    const auto replies = _replies;
    _replies.clear();
    for (const auto& reply : replies) {
        if (reply) {
            reply->abort();
            reply->deleteLater();
        }
    }
}

void UomReportClient::cancel(quint64 requestId)
{
    if (const auto reply = _replies.take(requestId)) {
        reply->abort();
        reply->deleteLater();
    }
}
