#include "RidActivationProvider.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

RidActivationProvider::RidActivationProvider(QObject* parent)
    : RidActivationProvider(QUrl(QStringLiteral("https://uom.amovlab.cn/api/v1/device/check-binding")), parent)
{}

RidActivationProvider::RidActivationProvider(const QUrl& endpoint, QObject* parent)
    : QObject(parent)
    , _endpoint(endpoint)
{}

bool RidActivationProvider::validSerial(const QString& serial)
{
    if (serial.size() != 20) {
        return false;
    }
    for (const QChar ch : serial) {
        if (ch.unicode() < 33 || ch.unicode() > 126) {
            return false;
        }
    }
    return true;
}

RidActivationResult RidActivationProvider::parseResponse(const QString& serial, int httpStatus, const QByteArray& body)
{
    const auto invalid = [](const QString& text) {
        return RidActivationResult{RidActivationResult::State::Unknown, text};
    };
    if (!validSerial(serial) || httpStatus < 200 || httpStatus >= 300 || body.size() > 16384) {
        return invalid(tr("Platform query failed"));
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject() ||
        !document.object().value(QStringLiteral("data")).isObject()) {
        return invalid(tr("Invalid platform response"));
    }
    const auto data = document.object().value(QStringLiteral("data")).toObject();
    if (!data.value(QStringLiteral("sn")).isString() || data.value(QStringLiteral("sn")).toString() != serial ||
        !data.value(QStringLiteral("bound")).isBool()) {
        return invalid(tr("Platform response does not match this aircraft"));
    }
    if (!data.value(QStringLiteral("bound")).toBool()) {
        return {RidActivationResult::State::Unbound,
                tr("Aircraft is not bound on the platform; RID state was retained")};
    }
    if (!data.value(QStringLiteral("unique_product_code")).isString() ||
        data.value(QStringLiteral("unique_product_code")).toString() != serial ||
        !data.value(QStringLiteral("activation_state")).isString()) {
        return invalid(tr("Invalid platform aircraft identity or activation state"));
    }
    const QString state = data.value(QStringLiteral("activation_state")).toString();
    if (state == QStringLiteral("1")) {
        return {RidActivationResult::State::Active, tr("Platform reports activated")};
    }
    if (state == QStringLiteral("2")) {
        return {RidActivationResult::State::Inactive, tr("Platform reports not activated")};
    }
    return invalid(state.isEmpty() ? tr("Platform has no reported activation state; RID state was retained")
                                   : tr("Unknown platform activation state; RID state was retained"));
}

void RidActivationProvider::query(quint64 generation, const QString& serial)
{
    cancel();
    _generation = generation;
    if (!validSerial(serial)) {
        emit resultReady(generation,
                         {RidActivationResult::State::Unknown, tr("A 20-character aircraft product code is required")});
        return;
    }
    QNetworkRequest request(_endpoint);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(5000);
    auto* reply = _network.post(
        request, QJsonDocument(QJsonObject{{QStringLiteral("sn"), serial}}).toJson(QJsonDocument::Compact));
    _reply = reply;
    connect(reply, &QNetworkReply::readyRead, this, [reply]() {
        if (reply->bytesAvailable() > 16384) {
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation, serial]() {
        const bool current = _reply == reply && _generation == generation;
        RidActivationResult result;
        if (reply->error() == QNetworkReply::NoError) {
            result = parseResponse(serial, reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
                                   reply->readAll());
        } else {
            result.detail = tr("Platform connection failed; RID state was retained");
        }
        reply->deleteLater();
        if (current) {
            _reply = nullptr;
            emit resultReady(generation, result);
        }
    });
}

void RidActivationProvider::cancel()
{
    ++_generation;
    if (_reply) {
        auto* reply = _reply.data();
        _reply = nullptr;
        reply->abort();
        reply->deleteLater();
    }
}
