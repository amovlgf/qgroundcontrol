#include "RidActivationSyncController.h"

RidActivationSyncController::RidActivationSyncController(QObject* parent)
    : RidActivationSyncController(QUrl(QStringLiteral("https://uom.amovlab.cn/api/v1/device/check-binding")), parent)
{}

RidActivationSyncController::RidActivationSyncController(const QUrl& endpoint, QObject* parent)
    : QObject(parent)
    , _provider(endpoint, this)
{
    connect(&_provider, &RidActivationProvider::resultReady, this,
            [this](quint64 generation, const RidActivationResult& result) {
                if (!_querying || generation != _generation) {
                    return;
                }
                _querying = false;
                if (result.writable()) {
                    emit targetReady(generation, _serial, result.state == RidActivationResult::State::Active);
                } else {
                    emit failed(result.detail);
                }
            });
}

void RidActivationSyncController::begin(quint64 generation, const QString& serial)
{
    cancel();
    _generation = generation;
    _serial = serial;
    _querying = true;
    _provider.query(generation, serial);
}

void RidActivationSyncController::cancel()
{
    _querying = false;
    _serial.clear();
    _provider.cancel();
}
